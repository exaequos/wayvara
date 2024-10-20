#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>

#include "../uxn.h"
#include "console.h"

#include <emscripten.h>

/*
Copyright (c) 2022-2023 Devine Lu Linvega, Andrew Alderwick

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE.
*/

#define WAYVARA_PATH "/tmp/wayvara.sock"

#define SYNTH_PATH "/tmp/synth.sock"

static int sock = -1;
static struct sockaddr_un local_addr, synth_addr;

static char midi_buf[3];
static char midi_off = 0;

int
console_input(Uxn *u, char c, int type)
{
	Uint8 *d = &u->dev[0x10];
	d[0x2] = c;
	d[0x7] = type;
	return uxn_eval(u, PEEK2(d));
}

void
console_listen(Uxn *u, int i, int argc, char **argv)
{
	for(; i < argc; i++) {
		char *p = argv[i];
		while(*p) console_input(u, *p++, CONSOLE_ARG);
		console_input(u, '\n', i == argc - 1 ? CONSOLE_END : CONSOLE_EOA);
	}
}

int create_socket() {

  int s;
  
  /* Create the server local socket */
  s = socket (AF_UNIX, SOCK_DGRAM, 0);
  if (s < 0) {
    return -1;
  }

  /* Bind server socket to NETFS_PATH */
  memset(&local_addr, 0, sizeof(local_addr));
  local_addr.sun_family = AF_UNIX;
  strcpy(local_addr.sun_path, WAYVARA_PATH);
  
  if (bind(s, (struct sockaddr *) &local_addr, sizeof(struct sockaddr_un))) {

    close(s);
    return -1;
  }

  memset(&synth_addr, 0, sizeof(synth_addr));
  synth_addr.sun_family = AF_UNIX;
  strcpy(synth_addr.sun_path, SYNTH_PATH);

  return s;
}

int send_to_synth(int s, Uint8 d) {

  int sent = 0;
  
  midi_buf[midi_off++] = d;

  if (midi_off == 3) {
    sent = sendto(s, midi_buf, midi_off, 0, (struct sockaddr *) &synth_addr, sizeof(synth_addr));

    midi_off = 0;

    //emscripten_log(EM_LOG_CONSOLE, "console: send to synth %d %d %d", midi_buf[0], midi_buf[1], midi_buf[2]);
  }

  return sent;
}

void
console_deo(Uint8 *d, Uint8 port)
{
  //emscripten_log(EM_LOG_CONSOLE, "console_deo: port=%d d=%d",port,d[port]);
  
	switch(port) {
	case 0x8:
	  //fputc(d[port], stdout);
	  //fflush(stdout);

	  if (sock < 0) {
	    sock = create_socket();

	    emscripten_log(EM_LOG_CONSOLE, "console: socket created %d", sock);
	  }
	  
	  if (sock >= 0)
	    send_to_synth(sock, d[port]);
		return;
	case 0x9:
		fputc(d[port], stderr);
		fflush(stderr);
		return;
	}
}
