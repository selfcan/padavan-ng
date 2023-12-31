/*
 * Copyright (c) 2018, Oracle America, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of "Oracle America, Inc." nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <netdb.h>
#include <netinet/in.h>

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <syslog.h>

#include <rpc/rpc.h>

#include "reentrant.h"
#include "rpc_com.h"

extern pthread_mutex_t port_lock;

/*
 * Dynamic port range as defined in RFC 6335 Section 6.
 * This range avoids all IANA-assigned service port
 * numbers.
 */
enum {
	LOWPORT		= 49152,
	ENDPORT		= 65534,
	NPORTS		= ENDPORT - LOWPORT + 1,
};

/*
 * This function decodes information about given port from provided array and
 * return if port is reserved or not.
 *
 * @reserved_ports an array of size at least "NPORTS / (8*sizeof(char)) + 1".
 * @port port number within range LOWPORT and ENDPORT
 *
 * Returns 0 if port is not reserved, non-negative if port is reserved.
 */
static int is_reserved(char *reserved_ports, int port) {
	port -= LOWPORT;
	if (port < 0 || port >= NPORTS)
		return 0;
	return reserved_ports[port/(8*sizeof(char))] & 1<<(port%(8*sizeof(char)));
}

/*
 * This function encodes information about given *reserved* port into provided
 * array. Don't call this function for ports which are not reserved.
 *
 * @reserved_ports an array of size at least "NPORTS / (8*sizeof(char)) + 1".
 * @port port number within range LOWPORT and ENDPORT
 *
 */
static void set_reserved(char *reserved_ports, int port) {
	port -= LOWPORT;
	if (port < 0 || port >= NPORTS)
		return;
	reserved_ports[port/(8*sizeof(char))] |= 1<<(port%(8*sizeof(char)));
}

/*
 * Parse local reserved ports obtained from
 * /proc/sys/net/ipv4/ip_local_reserved_ports into bit array.
 *
 * @reserved_ports a zeroed array of size at least
 * "NPORTS / (8*sizeof(char)) + 1". Will be used for bit-wise encoding of
 * reserved ports.
 *
 * On each call, reserved ports are read from /proc and bit-wise stored into
 * provided array
 *
 * Returns 0 on success, -1 on failure.
 */

static int parse_reserved_ports(char *reserved_ports) {
	int from=0, to;
	char delimiter = ',';
	int res;
	FILE * file_ptr = fopen("/proc/sys/net/ipv4/ip_local_reserved_ports","r");
	if (file_ptr == NULL) {
		(void) syslog(LOG_ERR,
			"Unable to open open /proc/sys/net/ipv4/ip_local_reserved_ports.");
		return -1;
	}
	do {
		if ((res = fscanf(file_ptr, "%d", &to)) != 1) {
			if (res == EOF) break;
			goto err;
		}
		if (delimiter != '-') {
			from = to;
		}
		for (int i = from; i <= to; ++i) {
			set_reserved(reserved_ports, i);
		}
	} while ((res = fscanf(file_ptr, "%c", &delimiter)) == 1);
	if (res != EOF)
		goto err;
	fclose(file_ptr);
	return 0;
err:
	(void) syslog(LOG_ERR,
		"An error occurred while parsing ip_local_reserved_ports.");
	fclose(file_ptr);
	return -1;
}

/*
 * Bind a socket to a dynamically-assigned IP port.
 *
 * @fd is an open but unbound socket.
 *
 * On each call, a port number is chosen at random from
 * within the dynamic/private port range, even if the
 * caller has CAP_NET_ADMIN_BIND.
 *
 * Returns 0 on success, -1 on failure. errno may be
 * set to a non-determinant value.
 *
 * This function is re-entrant.
 */
int __binddynport(int fd)
{
	struct sockaddr_storage ss;
#ifdef INET6
	struct sockaddr_in6 *sin6;
#endif
	struct sockaddr_in *sin;
	static unsigned int seed;
	in_port_t port, *portp;
	struct sockaddr *sap;
	socklen_t salen;
	int i, res, array_size;
	char *reserved_ports = NULL;

	if (__rpc_sockisbound(fd))
		return 0;

	res = -1;
	sap = (struct sockaddr *)(void *)&ss;
	salen = sizeof(ss);
	memset(sap, 0, salen);

	mutex_lock(&port_lock);

	if (getsockname(fd, sap, &salen) == -1)
		goto out;

	switch (ss.ss_family) {
	case AF_INET:
		sin = (struct sockaddr_in *)(void *)&ss;
		portp = &sin->sin_port;
		salen = sizeof(struct sockaddr_in);
		break;
#ifdef INET6
	case AF_INET6:
		sin6 = (struct sockaddr_in6 *)(void *)&ss;
		portp = &sin6->sin6_port;
		salen = sizeof(struct sockaddr_in6);
		break;
#endif
	default:
		goto out;
	}

	if (!seed) {
		struct timeval tv;

		gettimeofday(&tv, NULL);
		seed = tv.tv_usec * getpid();
	}
	array_size = NPORTS / (8*sizeof(char)) + 1;
	reserved_ports = malloc(array_size);
	if (!reserved_ports) {
		goto out;
	}
	memset(reserved_ports, 0, array_size);
	if (parse_reserved_ports(reserved_ports) < 0)
		goto out;

	port = (rand_r(&seed) % NPORTS) + LOWPORT;
	for (i = 0; i < NPORTS; ++i) {
		*portp = htons(port);
		if (!is_reserved(reserved_ports, port++)) {
			res = bind(fd, sap, salen);
			if (res >= 0) {
				res = 0;
				break;
			}
			if (errno != EADDRINUSE)
				break;
		}
		if (port > ENDPORT)
			port = LOWPORT;
	}

out:
	free(reserved_ports);
	mutex_unlock(&port_lock);
	return res;
}
