curlhttpfs :: FUSE HTTP Filesystem
==================================

Example:

    $ make curlhttpfs
    gcc -Wall -I/usr/include/fuse/ -D_FILE_OFFSET_BITS=64 curlhttpfs.c -o curlhttpfs -lcurl -lpthread -lfuse

    $ mkdir remote
    $ ./curlhttpfs -o base_url=http://tthsum.devs.nu,ro remote
    $ mount | tail -n1
    curlhttpfs on .../curlhttpfs/remote type fuse.curlhttpfs (ro,nosuid,nodev,user=walter)

    $ cat remote/index.html | file -
    /dev/stdin: HTML document, ASCII text

    $ wget -qO- http://tthsum.devs.nu/pkg/tthsum-1.2.0-win32-bin.zip | md5sum
    a8c756a3ae93732d93380fb383754dce  -
    $ md5sum remote/pkg/tthsum-1.2.0-win32-bin.zip
    a8c756a3ae93732d93380fb383754dce  remote/pkg/tthsum-1.2.0-win32-bin.zip

Bugs/Limitations:

  * Due to the way the kernel traverses the filesystem (using getattr on
    individual path elements) we have no clean way to discern files from
    directories. Right now we use the period ('.') as heuristic. If there
    is a period in the path-element, we treat it as a file; otherwise it
    is a directory.

  * Many webservers do not handle HTTP range requests. (Especially for
    dynamically generated content.) Right now support for those servers
    is broken.
 
  * Getting large files with small block sizes, yields a tremendous
    amount of work for both the server and the client. Compare these:

        $ dd if=remote/pkg/tthsum-1.2.0-win32-bin.zip of=tmp.log
        313+1 records in
        313+1 records out
        160636 bytes (161 kB) copied, 1.01863 s, 158 kB/s

        $ dd if=remote/pkg/tthsum-1.2.0-win32-bin.zip of=tmp.log bs=1M
        0+1 records in
        0+1 records out
        160636 bytes (161 kB) copied, 0.0257208 s, 6.2 MB/s

  * The filesystem should be mounted 'ro' automatically.

  * Vim(1) will create an seemingly endless loop trying to write
    swapfiles with different names on the RO-filesystem. Don't try to
    write any files there.

  * There are a few unchecked buffers here and there, they should be
    fixed: strcpy, strcat, sprintf. Buffer length of URL should be
    increased. Structure names and properties could be refactored for
    better naming and clarity.

  * Logging should be optional (mount option).

  * Should add curl options to specify timeouts and think about how to
    handle those.
