URL = http://tthsum.devs.nu

.PHONY: test clean mount umount

test: mount
	# OBSERVE: Filenames with a period ('.') in them are treated as files;
	# those without are treated as directories.
	@echo
	@echo Doing md5sum on $(URL)/pkg/tthsum-1.2.0-win32-bin.zip
	@echo
	md5sum remote/pkg/tthsum-1.2.0-win32-bin.zip
	@echo
	@echo Compare with md5sum as shown on index page:
	@echo
	w3m remote/index.html | grep win32

curlhttpfs: curlhttpfs.c
	gcc -Wall -I/usr/include/fuse/ -D_FILE_OFFSET_BITS=64 curlhttpfs.c -o curlhttpfs -lcurl -lpthread -lfuse

clean: umount
	$(RM) curlhttpfs
	-rmdir remote

mount: curlhttpfs umount
	@mkdir -p remote
	./curlhttpfs -o base_url=$(URL) remote

umount:
	-fusermount -uz remote/
