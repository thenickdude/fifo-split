# fifo-split

**n.b. this package does not have any tests yet**, I wouldn't suggest that anybody use it in anger until I've added some 
tests.

This tool allows you to split up a stream into multiple FIFO chunk streams, according to a specified
chunk size, without using any additional storage space. 

`fifo-split` only makes a single pass over the incoming stream, so each FIFO chunk is filled 
in order, and chunks must be consumed in order for `fifo-split` to advance to the next chunk.

`fifo-split` only supports POSIX operating systems (it might run under WSL2, I haven't tried it myself).

As new FIFO chunk files are created, `fifo-split` prints the new chunk's filename to stdout, which allows you to use 
xargs to consume chunks like so:

```bash
cat /dev/urandom \
  | fifo-split -0 --chunk-size 10MB --prefix "my-backup-" \
  | xargs -0 -I{} sh -c 'cp "$1" "/mnt/backups/$1"' -- {}
```

Creates a series of FIFOs (`my-backup-0`, `my-backup-1`, ...) in the current directory, which are then copied as regular 
files to `/mnt/backups/`.

Or else you can skip using `xargs` and instead manually consume the FIFO files yourself in-order in a different shell, 
e.g.

```bash
$ cat /dev/urandom | fifo-split --chunk-size 10MB --expected-size 100MB

Preallocated FIFO for chunk 0 at "chunk0"
Preallocated FIFO for chunk 1 at "chunk1"
...
chunk0
chunk1
...
```

In a separate shell you can consume those chunks in-order:

```bash
cp chunk0 /mnt/backups/my-backup-0
cp chunk1 /mnt/backups/my-backup-1
...
```

If you don't supply an `--expected-size` then it will only create one chunk FIFO at a time (i.e.
only `chunk0` will appear on the filesystem, until that chunk has been completely read, then `chunk1` will
appear next). For manual use this is a little inconvenient since it doesn't allow you to queue up the processing of
all of your chunks in advance.

If your `--expected-size` estimate is incorrect this is fine, `fifo-split` will automatically create additional FIFO
chunks as needed in order to take care of any overage (but this only happens once the stream advances to that point,
they will not be available immediately).

Note that so far these examples are no more powerful than using the [split](https://linux.die.net/man/1/split) command, 
because we've assumed that all the chunks will simultaneously fit into a single local destination directory and don't 
require any further processing, but see the next example for a more advanced pipeline. 

Run `fifo-split --help` to see an explanation of the available settings, reproduced here:

```
Splits a stream up into multiple FIFO chunk files, reads your stream from stdin.

Options:
  --help                shows this page
  --chunk-size arg      size of chunks to divide input stream into (e.g. 5GB,
                        8MiB, 700000B, required)
  --expected-size arg   expected total size of stream, so that chunk FIFOs can
                        be preallocated (e.g. 4.5TiB, optional)
  --prefix arg (=chunk) prefix of filename for chunk FIFOs to generate
  --only-chunks arg     only output chunks with specified indexes, comma
                        separated list of ranges (e.g. 0,5,10-)
  --skip-chunks arg     skip chunks with specified indexes, comma separated
                        list (e.g. -5,7,13-)
  -0 [ --print0 ]       use nul characters instead of newlines to separate
                        chunk filenames in output (for use with 'xargs -0')
```

## Worked example: Chunking a ZFS send stream to Amazon S3

**Note that this is an advanced method,** you had better understand what you're doing or else 
you could lose your data. Responsibility is all yours, and a backup isn't successful until 
you've also validated the restore process.

If you want to send a very large ZFS send stream to a simple object storage system (such as Amazon S3),
you might run into troubles with your Internet connection dropping causing the whole upload to 
be aborted, or you might run into maximum object size limitations (5TB in the case of S3).
In my case I want to upload to Backblaze B2, which offers an S3-compatible API.

Chunking up the stream solves both of those issues, but if you don't have enough space to locally store a copy of your 
ZFS send stream, it's difficult to chunk it into pieces to make it more tractable for upload.

This is where `fifo-split` comes in, it is able to chunk up the stream without using any extra local storage
space, making one chunk at a time available for upload to S3.

First I snapshotted my pool:

```bash
SOURCE=tank@my-backup
zfs snapshot -r "${SOURCE}"
```

I decided on a chunk size of 200GB (200,000,000,000 bytes). I used `pv` to get a progress bar for each upload, and I 
wanted to compress and encrypt each chunk, so I piped them through `zstd` and `gpg`, before finally passing them to 
the [AWS CLI's](https://aws.amazon.com/cli/) `aws s3 cp` command. You can omit the `zstd` and `gpg` parts if you don't
need compression or encryption, and you can replace `pv` with `cat` if you don't need a progress bar:

```bash
zfs send -R "${SOURCE}" \
  | fifo-split -0 --chunk-size 200GB --prefix "${SOURCE}." \
  | xargs -0 -I{} sh -c 'pv "$1" | zstd | gpg -z 0 --encrypt --recipient n.sherlock@gmail.com | aws s3 cp --expected-size 200000000000 --endpoint-url=https://s3.us-east.backblazeb2.com - "s3://my_backups/$1.zfs.zstd.gpg"' -- {}
```

**If the upload of a chunk fails**, `fifo-split` will fast-forward past the end of the aborted chunk for you 
automatically, and you can come back to re-upload that chunk later. 

**To retry chunks** we re-run the upload command to regenerate the ZFS send stream, but this time we ask
`fifo-split` to only give us the chunks we're interested in using the `--only-chunks` option.

For example:

```bash
zfs send -R "${SOURCE}" \
  | fifo-split --only-chunks 5,13,23 -0 --chunk-size 200GB --prefix "${SOURCE}." \
  | xargs -0 -I{} sh -c 'pv "$1" | zstd | gpg -z 0 --encrypt --recipient n.sherlock@gmail.com | aws s3 cp --expected-size 200000000000 --endpoint-url=https://s3.us-east.backblazeb2.com - "s3://my_backups/$1.zfs.zstd.gpg"' -- {}
```

`fifo-split` will fast-forward through the stream until it reaches the chunks of interest, and only write to and upload 
the FIFOs with those indices. Note that "fast-forwarding" is a bit of a misnomer, since skipping chunks takes the same 
amount of time as `zfs send > /dev/null` (but this'll still be much faster than re-uploading every chunk to S3!).

```bash
Preallocated FIFO for chunk 5 at "tank@my-backup.5"
Preallocated FIFO for chunk 13 at "tank@my-backup.13"
Preallocated FIFO for chunk 23 at "tank@my-backup.23"
Skipping chunk 0...
Skipping chunk 1...
Skipping chunk 2...
Skipping chunk 3...
Skipping chunk 4...
```

**This is only possible** because `zfs send` produces the same bitstream each time it is sent (at least within a ZFS version),
so we are able to re-run the `zfs send` command to reproduce the stream we failed to upload.

**But note that this is not the case for ZFS before version 2.1.8 with the embedded blocks option enabled** (i.e. send using 
`--raw` or `-e`), due to a bug in `zfs send` which fails to properly initialise the padding memory at the end of an 
embedded block, causing those bytes to take on random values:

https://github.com/openzfs/zfs/issues/13778

These raw streams do not have a consistent checksum, and therefore resuming the upload will create a backup with
inconsistent internal checksums that ZFS will refuse to receive. You'd have to patch `zfs recv` to ignore bad stream
checksums to receive such a backup. Upgrade to 2.1.8 or later before sending.

### Restoring from this chunked zfs send-stream

To download and restore from this chunked stream, we can create a FIFO file to receive each chunk into, 
then join those together in order with `pv` or `cat`, and start receiving them to their 
destination:

```bash
NUM_CHUNKS=23

for ((i=0; i < NUM_CHUNKS; i++))
do
  rm -f "chunk${i}"
  mkfifo "chunk${i}"
done


pv $(eval "echo chunk{0..$((NUM_CHUNKS - 1))}") | zfs recv tank/destination
```

Now `zfs recv` hangs waiting for the first FIFO to begin being filled up.

In a separate shell, we can start downloading chunks from S3 and piping them into the chunk 
FIFOs (be sure to load those variables like `SOURCE` and `NUM_CHUNKS` into this new shell first). We unwrap 
the encryption and compression we applied before we give the result to zfs:

```bash
for ((i=0; i < NUM_CHUNKS; i++))
do
  aws s3 cp --endpoint-url=https://s3.us-east.backblazeb2.com "s3://my_backups/${SOURCE}.${i}.zfs.zstd.gpg" - | gpg2 --decrypt | zstd -d > "chunk${i}"
done
```

Once the first download starts going, ZFS starts receiving that data and writing it to the 
destination immediately, and you'll see the progress bar start moving in the shell that's 
running `zfs recv`.

We need to download the files in order, since ZFS will read them in order. Downloading a chunk 
before ZFS starts reading it won't cause a failure, but the download will stall waiting for ZFS 
to catch up to it.

This FIFO approach doesn't consume any additional temporary space, your data is written directly 
to its destination on the target pool.

**If you did have spare space**, you could skip the FIFO and download the chunks the regular way:</p>

```bash
for ((i=0; i < NUM_CHUNKS; i++))
do
  rm -f "chunk${i}"
  aws s3 cp --endpoint-url=https://s3.us-east.backblazeb2.com "s3://my_backups/${SOURCE}.${i}.zfs.zstd.gpg" - | gpg2 --decrypt | zstd -d > "chunk${i}"
done

pv $(eval "echo chunk{0..$((NUM_CHUNKS - 1))}") | zfs recv tank/destination
```

Doing it this way gives you the opportunity to retry any failed downloads before running the "zfs recv" command.

## Building fifo-split

Building requires a C++17 compiler, cmake 3.18+, make, and [Conan 1.47+](https://conan.io/) 
to be installed. 

Run `make`to build it, the executable will be written to `build/fifo-split`.

I only tested building it on macOS 12 Monterey and Debian 11.