# Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

CC?=cc
CFLAGS+=-Werror -Wall
LDFLAGS+=-lasound
BINARY=jabra_vold

OBJS=jabra.o

all: $(BINARY)

$(BINARY): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

clean:
	rm -f *.o $(BINARY)
