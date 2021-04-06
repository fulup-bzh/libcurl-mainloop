#
# Copyright (C) 2021 "IoT.bzh"
# Author "Fulup Ar Foll" <fulup@iot.bzh>
#
# Use of this source code is governed by an MIT-style
# license that can be found in the LICENSE file or at https://opensource.org/licenses/MIT.
# $RP_END_LICENSE$
#

ifeq ($(MAIN_LOOP),systemd)
	GLUE_OPTS = -DGLUE_LOOP_ON
	GLUE_FUNC = build/glue-systemd.o
	GLUE_LIB=libsystemd

else ifeq ($(MAIN_LOOP),libuv)
	GLUE_LIB=libuv
	GLUE_OPTS = -DGLUE_LOOP_ON
	GLUE_FUNC = build/glue-libuv.o

else
	MAIN_LOOP = epoll
	GLUE_LIB=
	GLUE_OPTS = -DGLUE_LOOP_ON
	GLUE_FUNC = build/glue-epool.o

endif

CFLAGS = -g $(shell pkg-config --cflags libcurl $(GLUE_LIB)) $(GLUE_OPTS)
LFLAGS = -g $(shell pkg-config --cflags --libs libcurl $(GLUE_LIB))

.PHONY: all clean

all: builddir build/http-client done

done:
	@echo "--"
	@echo "-- syntax: ./build/http-client -v -a https://example.com http://example.com"
	@echo "--"

build/http-client: build/http-client.o build/curl-main.o $(GLUE_FUNC)
	$(CC) $(LFLAGS) -o $@ build/http-client.o build/curl-main.o $(GLUE_FUNC) $(LFLAGS)

build/glue-%.o: event-loops/glue-%.c http-client.h
	$(CC) $(CFLAGS) $(GLUE_OPTS) -c ./$< -o $@

build/%.o: %.c http-client.h
	$(CC) $(CFLAGS) $(GLUE_OPTS) -c ./$< -o $@

builddir:
	@mkdir -p ./build

clean:
	rm build/* 2>/dev/null || true

help:
	@echo "[missing-maonloop] syntax: 'make MAIN_LOOP=systemd|libuv'"

