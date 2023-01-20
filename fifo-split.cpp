#include <iostream>
#include <cstdint>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>

#include <boost/program_options.hpp>
#include <boost/spirit/home/x3.hpp>
#include <UnitConvert.hpp>

#include "chunk-range.hpp"

namespace po = boost::program_options;

using boost::numeric_cast;

static void print_usage(po::options_description &options) {
    std::cerr << "fifo-split" << std::endl << std::endl;
    std::cerr << "Splits a stream up into multiple FIFO chunk files, reads your stream from stdin." << std::endl << std::endl;
    std::cerr << options << std::endl;
}

class write_error : std::runtime_error {
public:
    int errorCode;
    
    write_error(int errorCode) : 
        std::runtime_error("Error writing to stream " + std::string(strerror(errorCode))), 
        errorCode(errorCode) {}
};

/**
 * Writes bytesToWrite bytes from the buffer into the stream.
 * 
 * If any bytes could not be written, a write_error exception is thrown.
 * 
 * @param buffer 
 * @param bytesToWrite 
 * @param stream 
 * 
 * @return 
 */
static void retryable_write(const char *buffer, ssize_t bytesToWrite, int stream) {
    while (bytesToWrite > 0) {
        ssize_t numWritten = write(stream, buffer, bytesToWrite);

        if (numWritten == -1) {
            if (errno == EINTR) {
                // Call was interrupted before any data was written, so we should retry:
                continue;
            }
            
            throw write_error(errno);
        } 
    
        bytesToWrite -= numWritten;
        buffer += numWritten;
    }
}

/**
 * Attempts to read bytesToRead bytes from the stream into the buffer, returning the number of bytes actually read.
 * 
 * If feof() is reached, the number of bytes read could be fewer than requested. For other read errors
 * an exception will be thrown.
 * 
 * @param buffer 
 * @param bytesToRead 
 * @param stream 
 * 
 * @return 
 */
static ssize_t retryable_read(char *buffer, ssize_t bytesToRead, int stream) {
    ssize_t totalBytesRead = 0;

    while (bytesToRead > 0) {
        ssize_t numRead = read(stream, buffer, bytesToRead);
        
        if (numRead == -1) {
            if (errno == EINTR) {
                // Call was interrupted before any data was read
                continue;
            }

            throw std::runtime_error("Error reading from stream " + std::string(strerror(errno)));
        }
        
        totalBytesRead += numRead;
        bytesToRead -= numRead;
        buffer += numRead;

        if (numRead == 0) {
            // Reached EOF
            break;
        }
    };
    
    return totalBytesRead;
}

/**
 * Throws an exception if a read error occurs, returns false if a write error occurs.
 * 
 * @param input File descriptor to read from
 * @param bytesToCopy Number of bytes to copy
 * @param totalBytesRead Will be set to the number of bytes read from input stream. This will 
 *                       be lower than requested if you hit EOF on the input stream.
 * @param output Destination stream, or -1 to discard read bytes
 * 
 * @return true if no write error was encountered  
 */
static bool copy_stream(int input, int64_t bytesToCopy, int64_t &totalBytesRead, int output = -1) {
    size_t BUFFER_SIZE = 128 * 1024;
    char buffer[BUFFER_SIZE];
    
    totalBytesRead = 0;

    while (bytesToCopy > 0) {
        auto bytesToRead = numeric_cast<ssize_t>(std::min(bytesToCopy, (int64_t) BUFFER_SIZE));
        
        ssize_t numBytesRead = retryable_read(buffer, bytesToRead, input);
        
        totalBytesRead += numeric_cast<int64_t>(numBytesRead);
        bytesToCopy -= numeric_cast<int64_t>(numBytesRead);

        if (output != -1) {
            try {
                retryable_write(buffer, numBytesRead, output);
            } catch (write_error &err) {
                return false;
            }
        }  

        if (numBytesRead != bytesToRead) {
            break; // We reached EOF of input stream early
        }
    }
    
    return true;
}

static void create_fifo(const std::string &fifoFilename) {
    int result = mkfifo(fifoFilename.c_str(), 0600);
    
    if (result != 0 && errno == EEXIST) {
        // Assume it's a defunct FIFO from a previous run, and remove it
        unlink(fifoFilename.c_str());

        // Try again:
        result = mkfifo(fifoFilename.c_str(), 0600);
    }
    
    if (result != 0) {
        throw std::runtime_error(
            "Failed to create FIFO " + fifoFilename + ", error code: " + std::to_string(errno));
    }
}

static int64_t chunk_stream(int stream, int64_t chunkSize, int64_t expectedSize, const std::string &chunkPrefix, const RangeList &onlyChunks, const RangeList &skipChunks, bool zeroSep) {
    auto shouldWriteChunk = [&](int chunkIndex) {
        return (onlyChunks.empty() || onlyChunks.contains(chunkIndex)) && !skipChunks.contains(chunkIndex);
    };
    
    bool overallFailure = false;
    int64_t totalCopied = 0;
    int expectedChunks = numeric_cast<int>((expectedSize + chunkSize - 1) / chunkSize);
    int lastChunkWanted;

    // Do we already know the index of the final chunk we will produce due to finite onlyChunks filters?
    if (!onlyChunks.empty() && !onlyChunks.containsPositiveInf() && onlyChunks.getLargestFiniteBound(lastChunkWanted)) {
        // This overrides the calculation based on expectedSize
        expectedChunks = lastChunkWanted + 1;
    } else {
        lastChunkWanted = -1; // Final chunk is unknown
    }

    // If the user explicitly referenced chunk indexes in onlyChunks, also preallocate FIFOs reaching that number
    int maxBound;
    if (onlyChunks.getLargestFiniteBound(maxBound)) {
        expectedChunks = std::max(expectedChunks, maxBound + 1);
    }

    // If we know approximately how many FIFOs we need, we can preallocate them now
    if (expectedChunks > 0) {
        for (int chunkIndex = 0; chunkIndex < expectedChunks; chunkIndex++) {
            if (shouldWriteChunk(chunkIndex)) {
                std::string fifoFilename = chunkPrefix + std::to_string(chunkIndex);
                create_fifo(fifoFilename);

                std::cerr << "Preallocated FIFO for chunk " << chunkIndex << " at \"" << fifoFilename << "\"" << std::endl;
            }
        }
    }

    // Get EPIPE errors from write() calls instead of killing our process, please:
    signal(SIGPIPE, SIG_IGN);

    for (int chunkIndex = 0; lastChunkWanted == -1 || chunkIndex <= lastChunkWanted; chunkIndex++) {
        int64_t bytesRead;

        if (shouldWriteChunk(chunkIndex)) {
            std::string fifoFilename = chunkPrefix + std::to_string(chunkIndex);
            
            if (chunkIndex >= expectedChunks) {
                // Since we didn't preallocate one
                create_fifo(fifoFilename);
            }

            std::cout << fifoFilename << (zeroSep ? std::string("\0", 1) : "\n") << std::flush;

            int output = open(fifoFilename.c_str(), O_WRONLY);
            if (output == -1) {
                throw std::runtime_error("Failed to open FIFO " + std::string(strerror(errno)));
            }
            
            bool writeSuccess = copy_stream(stream, chunkSize, bytesRead, output);

            close(output);

            if (!writeSuccess) {
                std::cerr << "Chunk " << chunkIndex << " was closed early by consumer. Skipping remainder of chunk..." << std::endl;

                overallFailure = true;

                int64_t skippedBytes;
                copy_stream(stream, chunkSize - bytesRead, skippedBytes);

                bytesRead += skippedBytes;
            }
        } else {
            std::cerr << "Skipping chunk " << chunkIndex << "..." << std::endl;
            copy_stream(stream, chunkSize, bytesRead);
        }

        totalCopied += bytesRead;

        if (bytesRead < chunkSize) {
            // We reached EOF in the input stream, so this was the last chunk in the input

            // But if there are preallocated FIFOs yet to be processed, keep going so that we can close those
            if (chunkIndex >= expectedChunks - 1) {
                break;
            }
        }
    }

    return totalCopied;
}

int main(int argc, char **argv) {
    po::options_description mainOptions("Options");
    mainOptions.add_options()
        ("help", "shows this page")
        ("chunk-size", po::value<std::string>()->required(),
            "size of chunks to divide input stream into (e.g. 5GB, 8MiB, 700000B, required)")
        ("expected-size", po::value<std::string>(),
            "expected total size of stream, so that chunk FIFOs can be preallocated (e.g. 4.5TiB, optional)")
        ("prefix", po::value<std::string>()->default_value("chunk"),
            "prefix of filename for chunk FIFOs to generate")
        ("only-chunks", po::value<std::string>(), "only output chunks with specified indexes, comma separated list of ranges (e.g. 0,5,10-)")
        ("skip-chunks", po::value<std::string>(), "skip chunks with specified indexes, comma separated list (e.g. -5,7,13-)")
        ("print0,0", "use nul characters instead of newlines to separate chunk filenames in output (for use with 'xargs -0')")
        ;

    po::variables_map vm;
    UnitConvert::UnitRegistry ureg;

    try {
        po::store(po::command_line_parser(argc, argv)
            .options(mainOptions).run(), vm);
        po::notify(vm);
    } catch (boost::program_options::required_option &e) {
        std::cerr << "Error: " << e.what() << std::endl << std::endl;
        
        print_usage(mainOptions);
        return EXIT_FAILURE;
    } catch (std::exception &e) {
        std::cerr << "Error parsing options: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    if (vm.count("help") || vm.count("chunk-size") == 0) {
        print_usage(mainOptions);
        return EXIT_FAILURE;
    }
    
    ureg.addUnit("B = [1]");
    ureg.addUnit("1 KB = 1024 B"); // Since "kB" is the only accepted way of writing kilobyte, but "KB" is not unusual
    ureg.addUnit("1 KiB = 1024 B");
    ureg.addUnit("1 MiB = 1024 KiB");
    ureg.addUnit("1 GiB = 1024 MiB");
    ureg.addUnit("1 TiB = 1024 GiB");
    ureg.addUnit("1 PiB = 1024 TiB");
    
    int64_t chunkSize, expectedSize;
    
    // TODO support unitless byte counts
    try {
        chunkSize = ureg.makeQuantity<int64_t>(vm["chunk-size"].as<std::string>()).to("B").value();
        
        if (chunkSize <= 0) {
            throw std::runtime_error("Chunk size must be positive! Ensure units are properly capitalised");
        }
        
        std::cerr << "Chunk size is " << std::to_string(chunkSize) << "B" << std::endl;
    } catch (std::exception &e) {
        std::cerr << "Invalid chunk-size: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    if (vm.count("expected-size")) {
        try {
            double expectedSizeDbl = ureg.makeQuantity<double>(vm["expected-size"].as<std::string>()).to("B").value();

            // Catch people specifying "5 millibytes":
            if (expectedSizeDbl < 1) {
                throw std::runtime_error("Expected size must be positive! Ensure units are properly capitalised");
            }

            expectedSize = numeric_cast<int64_t>(std::ceil(expectedSizeDbl));
        } catch (std::exception &e) {
            std::cerr << "Invalid expected-size: " << e.what() << std::endl;
            return EXIT_FAILURE;
        }
    } else {
        expectedSize = 0;
    }
    
    RangeList onlyChunks, skipChunks;
    
    if (vm.count("only-chunks")) {
        const auto &list = vm["only-chunks"].as<std::string>();
        onlyChunks.parse(list.begin(), list.end());
    }
    if (vm.count("skip-chunks")) {
        const auto &list = vm["skip-chunks"].as<std::string>();
        skipChunks.parse(list.begin(), list.end());
    }
    
    int64_t totalBytes = chunk_stream(fileno(stdin), chunkSize, expectedSize, vm["prefix"].as<std::string>(), onlyChunks, skipChunks, vm.count("print0") > 0);
    
    std::cerr << "Total stream size was " << std::to_string(totalBytes) << " bytes" << std::endl;
    
    std::cerr << "Done!" << std::endl;
}