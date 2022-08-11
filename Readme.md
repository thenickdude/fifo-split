# fifo-split

This tool allows you to split up a stream into multiple FIFO chunk streams, according to a specified
chunk size, without using any additional storage space. 

`fifo-split` only makes a single pass over the incoming stream, so each FIFO chunk is filled 
in order, and chunks must be consumed in order for `fifo-split` to advance to the next chunk.

`fifo-split` only supports POSIX operating systems (it might run under WSL2, I haven't tried it myself).

Run `./fifo-split --help` to see an explanation of the available settings.

## Worked example: Chunking a ZFS send stream

**Note that this is an advanced method,** you had better understand what you're doing or else 
you could lose your data. Responsibility is all yours, and a backup isn't successful until 
you've also validated the restore process.

If you want to send a very large ZFS send stream to a simple object storage system (such as Amazon S3),
you might run into troubles with your Internet connection dropping causing the whole upload to 
be aborted, or you might run into maximum object size limitations (5TB in the case of S3).

Chunking up the stream solves both of those issues, but if you don't have enough space to locally store a copy of your 
ZFS send stream, it's difficult to chunk it into pieces to make it more tractable for upload.

This is where `fifo-split` comes in, it is able to chunk up the stream without using any extra local storage
space, making one chunk at a time available for upload to S3.

First I snapshotted my pool:

```bash
zfs snapshot -r tank@my-backup
```

I decided on a chunk size of 200GB (200,000,000,000 bytes), and checked the expected size of 
my pool using `zfs list` (4.5TB). Then I ran this command to begin chunking the stream:

```bash
zfs send --raw -R tank@my-backup | ./fifo-split --chunk-size 200GB --expected-size 4.5TB
```

`fifo-split` creates a series of FIFO files on disk (`chunk0`, `chunk1`, ...) according to the number of 
chunks expected given the `expected-size` you gave it. These chunks will become available in order, i.e.
reading from `chunk1` will stall until `chunk0` has been completely read.

If you don't supply an `expected-size` then it only creates one chunk FIFO at a time (i.e. 
only `chunk0` will appear on the filesystem, until that chunk has been completely read, then `chunk1` will
appear next). This is a little inconvenient since it doesn't allow you to queue up the upload of
all of your chunks in advance.

If your `expected-size` estimate is incorrect this is fine, `fifo-split` will automatically create additional FIFO
chunks as needed in order to take care of any overage (but this only happens once the stream advances to that point,
they will not be available immediately).

Now I can begin uploading these chunks to S3 (actually, in my case to Backblaze B2's S3-compatible API).

I used a little shell script to generate the upload commands for me so that I could run them myself manually at
my leisure. You can adapt this script for your own uses.

I used `pv` to get a progress bar for each upload, and I wanted to compress and encrypt each chunk,
so I piped them through `zstd` and `gpg`, before finally passing them to the [AWS CLI's](https://aws.amazon.com/cli/) 
`aws s3 cp` command. You can omit the `zstd` and `gpg` parts if you don't need compression or 
encryption, and you can replace `pv` with `cat` if you don't need a progress bar:

```bash
SOURCE=tank@my-backup
NUM_CHUNKS=23
CHUNK_SIZE=200000000000

for ((i=0; i < NUM_CHUNKS; i++))
do
  echo pv "chunk${i}" \| zstd \| gpg -z 0 --encrypt --recipient n.sherlock@gmail.com \| aws s3 cp --expected-size ${CHUNK_SIZE} --endpoint-url=https://s3.us-east.backblazeb2.com - "s3://my-backups/${SOURCE}.zfs.${i}.zstd.gpg" \&\& echo chunk${i} upload success \|\| echo chunk${i} upload failed
done
```

This printed out a series of upload commands for me:

```bash
pv chunk0 | zstd | gpg -z 0 --encrypt --recipient n.sherlock@gmail.com | aws s3 cp --expected-size 200000000000 --endpoint-url=https://s3.us-east.backblazeb2.com - s3://my-backups/tank@my-backup.zfs.0.zstd.gpg && echo chunk0 upload success || echo chunk0 upload failed
pv chunk1 | zstd | gpg -z 0 --encrypt --recipient n.sherlock@gmail.com | aws s3 cp --expected-size 200000000000 --endpoint-url=https://s3.us-east.backblazeb2.com - s3://my-backups/tank@my-backup.zfs.1.zstd.gpg && echo chunk1 upload success || echo chunk1 upload failed
pv chunk2 | zstd | gpg -z 0 --encrypt --recipient n.sherlock@gmail.com | aws s3 cp --expected-size 200000000000 --endpoint-url=https://s3.us-east.backblazeb2.com - s3://my-backups/tank@my-backup.zfs.2.zstd.gpg && echo chunk2 upload success || echo chunk2 upload failed
```

So now I can run these one-by-one, in order, to upload my chunks.

**If the upload of a chunk fails**, let's say `chunk5`, you need to finish consuming that chunk before `fifo-split` can 
advance to the next chunk, like so:

```bash
pv chunk5 > /dev/null
```

Continue with the rest of your chunk uploads, and we'll retry any failed chunks afterwards.

**To retry chunks** we regenerate the ZFS send stream and ask `fifo-split` to only give us the chunks we're interested in
using the `--only-chunks` option.

This is only possible because `zfs send` produces the same bitstream each time it is sent (at least within a ZFS version), 
so we're able to re-run the `zfs send` command to reproduce the stream we failed to upload.

For example:

```bash
zfs send --raw -R tank@my-backup | ./fifo-split --chunk-size 200GB --expected-size 4.5TB --only-chunks 5,13,23
```

`fifo-split` will fast-forward through the stream until it reaches the chunks of interest, and only write to the FIFOs 
with those indices. Note that "fast-forwarding" is a bit of a misnomer, since skipping chunks takes the same amount of time
as `zfs send > /dev/null` (but this'll still be much faster than re-uploading every chunk to S3!).

```bash
Preallocated FIFO for chunk 5 at "chunk5"
Preallocated FIFO for chunk 13 at "chunk13"
Preallocated FIFO for chunk 23 at "chunk23"
Skipping chunk 0...
Skipping chunk 1...
Skipping chunk 2...
Skipping chunk 3...
Skipping chunk 4...
Copying chunk 5 to "chunk5"...
```

Now we can re-upload those chunk FIFOs (chunk5, chunk13, chunk23) using the same upload commands we generated before.

### Restoring from this chunked zfs send-stream

To download and restore from this chunked stream, we can create a FIFO file to receive each chunk into, 
then join those together in order with `pv` or `cat`, and start receiving them to their 
destination:

```bash
for ((i=0; i < NUM_CHUNKS; i++))
do
  rm -f "chunk${i}"
  mkfifo "chunk${i}"
done


pv $(eval "echo chunk{0..$((NUM_CHUNKS - 1))}") | zfs recv tank/destination
```

Now `zfs recv` hangs waiting for the first FIFO to begin being filled up.

In a separate shell, we can start downloading chunks from S3 and piping them into the chunk 
FIFOs (be sure to load those variables like NUM_CHUNKS into this new shell first). We unwrap 
the encryption and compression we applied before we give the result to zfs:

```bash
for ((i=0; i &lt; NUM_CHUNKS; i++))
do
  aws s3 cp --endpoint-url=https://s3.us-east.backblazeb2.com "s3://my-backups/${SOURCE}.zfs.${i}.zstd.gpg" - | gpg2 --decrypt | zstd -d > "chunk${i}"
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
  aws s3 cp --endpoint-url=https://s3.us-east.backblazeb2.com "s3://my-backups/${SOURCE}.zfs.${i}.zstd.gpg" - | gpg2 --decrypt | zstd -d > "chunk${i}"
done

pv $(eval "echo chunk{0..$((NUM_CHUNKS - 1))}") | zfs recv tank/destination
```

Doing it this way gives you the opportunity to retry any failed downloads before running the "zfs recv" command.

## Building fifo-split

Building requires a C++17 compiler, cmake 3.18+, make, and [Conan 1.47+](https://conan.io/) 
to be installed. 

Run `make`to build it, the executable will be written to `build/fifo-split`.

I only tested building it on macOS 12 Monterey and Debian 11.