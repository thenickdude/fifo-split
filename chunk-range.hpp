#pragma once

#include <boost/spirit/home/x3.hpp>
#include <boost/fusion/include/adapt_struct.hpp>

typedef struct chunk_range {
    int start;
    int end;
    bool haveStart;
    bool haveEnd;

    chunk_range() {
        start = 0;
        end = 0;
        haveStart = false;
        haveEnd = false;
    }
} chunk_range;

BOOST_FUSION_ADAPT_STRUCT(
    chunk_range,
    start, end
);

namespace client {
    namespace parser {
        namespace x3 = ::boost::spirit::x3;
        namespace ascii = x3::ascii;

        using x3::int_;
        using x3::lit;
        using x3::_val;
        using x3::_attr;
        using x3::eps;

        auto captureStart = [](auto& ctx) {
            _val(ctx).start = _attr(ctx);
            _val(ctx).haveStart = true;
        };

        auto captureEnd = [](auto& ctx) {
            _val(ctx).end = _attr(ctx);
            _val(ctx).haveEnd = true;
            
            if (_val(ctx).end < _val(ctx).start) {
                throw new std::runtime_error("End of range cannot be before start");
            }
        };

        auto captureSingle = [](auto& ctx) {
            _val(ctx).end = _val(ctx).start;
            _val(ctx).haveEnd = true;
        };

        x3::rule<class chunk_range_p, struct chunk_range> const chunk_range_p = "chunk_range";

        auto const chunk_range_p_def =
            (
                int_ [captureStart]
                    >> (
                        ('-' >> -(int_ [captureEnd]))
                            | (eps [captureSingle])
                    )
            ) | (
                '-' >> int_ [captureEnd]
            );

        BOOST_SPIRIT_DEFINE(chunk_range_p);
    }
}

/**
 * Represents a list of ranges, which can have finite or infinite boundaries, e.g. 2-2,5-9,11- is a valid set of ranges, 
 * and includes (for instance) 2, and all numbers from 11 onwards, but not 1 or 10.
 */
class RangeList {
private:
    std::vector<chunk_range> ranges;

public:
    template <typename Iterator>
    bool parse(const Iterator begin, const Iterator end) {
        using boost::spirit::x3::_attr;
        using boost::spirit::x3::ascii::space;
        
        phrase_parse(
            begin, 
            end,
            client::parser::chunk_range_p % ',',
            space,
            ranges
        );

        return begin == end;
    }

    bool contains(int i) const {
        for (chunk_range range : ranges) {
            if (!(range.haveStart && i < range.start || range.haveEnd && i > range.end)) {
                return true;
            }
        }
        return false;
    }
    
    bool containsPositiveInf() const {
        return std::any_of(ranges.cbegin(), ranges.cend(), [](chunk_range range){ return !range.haveEnd; });
    }

    bool containsNegativeInf() const {
        return std::any_of(ranges.cbegin(), ranges.cend(), [](chunk_range range){ return !range.haveStart; });
    }

    /**
     * Get the smallest finite boundary in the set (start or end)
     * 
     * @param result 
     * @return true if there was a finite boundary, otherwise false and the value of "result" is not well-defined 
     */
    bool getSmallestFiniteBound(int &result) const {
        int resultValid = false;
        
        for (chunk_range range: ranges) {
            if (range.haveStart) {
                result = resultValid ? std::min(result, range.start) : range.start;
                resultValid = true;
            }
            if (range.haveEnd) {
                result = resultValid ? std::min(result, range.end) : range.end;
                resultValid = true;
            }
        }
        
        return resultValid;
    }

    /**
     * Get the largest finite boundary in the set (start or end)
     * 
     * @param result 
     * @return true if there was a finite boundary, otherwise false and the value of "result" is not well-defined 
     */
    bool getLargestFiniteBound(int &result) const {
        int resultValid = false;

        for (chunk_range range: ranges) {
            if (range.haveStart) {
                result = resultValid ? std::max(result, range.start) : range.start;
                resultValid = true;
            }
            if (range.haveEnd) {
                result = resultValid ? std::max(result, range.end) : range.end;
                resultValid = true;
            }
        }

        return resultValid;
    }

    bool empty() const {
        return ranges.empty();
    }
    
    void printRanges() const {
        for (chunk_range range : ranges) {
            if (range.haveStart && range.haveEnd && range.start == range.end) {
                std::cout << range.start;
            } else {
                if (range.haveStart) {
                    std::cout << range.start;
                }

                std::cout << "-";

                if (range.haveEnd) {
                    std::cout << range.end;
                }
            }

            std::cout << std::endl;
        }
    }
};
