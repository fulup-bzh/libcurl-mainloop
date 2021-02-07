#
# Copyright (C) 2021 "IoT.bzh"
# Author "Fulup Ar Foll" <fulup@iot.bzh>
#
# Use of this source code is governed by an MIT-style
# license that can be found in the LICENSE file or at https://opensource.org/licenses/MIT.
# $RP_END_LICENSE$
#


CFLAGS = -g $(shell pkg-config --cflags ) \
	-Wl,--version-script=$(shell pkg-config --variable=version_script libcurl libsystemd)

LFLAGS = -g $(shell pkg-config --cflags --libs libcurl libsystemd)


.PHONY: all clean

all: builddir curl-http done

done:
	@echo "--"
	@echo "-- syntax: ./curl-http http://example.com https://example.com"
	@echo "--"

curl-http: build/curl-http.o build/curl-main.o
	$(CC) $(LFLAGS) -o $@ build/curl-http.o build/curl-main.o

build/%.o: %.c
	$(CC) $(CFLAGS) -c ./$< -o $@

builddir:
	@mkdir -p ./build

clean:
	rm build/* 2>/dev/null || true

