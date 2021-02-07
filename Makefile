#
# Copyright (C) 2021 "IoT.bzh"
# Author "Fulup Ar Foll" <fulup@iot.bzh>
#
# Use of this source code is governed by an MIT-style
# license that can be found in the LICENSE file or at https://opensource.org/licenses/MIT.
# $RP_END_LICENSE$
#

# mainloop waiting time in seconds
OPTIONS = -D MAIN_LOOP_WAIT=3

CFLAGS = -g $(shell pkg-config --cflags libcurl libsystemd) ${OPTIONS}
LFLAGS = -g $(shell pkg-config --cflags --libs libcurl libsystemd)


.PHONY: all clean

all: builddir build/curl-http done

done:
	@echo "--"
	@echo "-- syntax: ./build/curl-http -v -a https://example.com http://example.com"
	@echo "--"

build/curl-http: build/curl-http.o build/curl-main.o
	$(CC) $(LFLAGS) -o $@ build/curl-http.o build/curl-main.o

build/%.o: %.c
	$(CC) $(CFLAGS) -c ./$< -o $@

builddir:
	@mkdir -p ./build

clean:
	rm build/* 2>/dev/null || true

