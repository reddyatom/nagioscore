/*****************************************************************************
 *
 * NETUTILS.C - Network connection  utility functions for Nagios
 *
 * Portions Copyright (c) 1999-2008 Nagios Plugin development team
 *
 * License:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *****************************************************************************/

#include "../include/config.h"
#include "../include/common.h"
#include "../include/netutils.h"

#ifdef HAVE_SSL
int my_ssl_connect(const char *host_name, int port, int *sd, SSL **ssl, SSL_CTX **ctx, int timeout) {
	struct addrinfo hints;
	struct addrinfo *res;
	int result;
	char port_str[6];
	int flags = 0;
	fd_set rfds;
	fd_set wfds;
	struct timeval tv;
	int optval;
	socklen_t optlen;

	const SSL_METHOD *method;
	int ssl_result;
	
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_INET;
	hints.ai_socktype = SOCK_STREAM;

	/* make sure our static port_str is long enough */
	if(port > 65535)
		return ERROR;

	snprintf(port_str, sizeof(port_str), "%d", port);
	result = getaddrinfo(host_name, port_str, &hints, &res);
	if(result != 0) {
		/*printf("GETADDRINFO: %s (%s) = %s\n",host_name,port_str,gai_strerror(result));*/
		return ERROR;
		}

	/* create a socket */
	*sd = socket(res->ai_family, SOCK_STREAM, res->ai_protocol);
	if(*sd < 0) {
		freeaddrinfo(res);
		return ERROR;
		}

	/* make socket non-blocking */
	flags = fcntl(*sd, F_GETFL, 0);
	fcntl(*sd, F_SETFL, flags | O_NONBLOCK);

	/* attempt to connect */
	result = connect(*sd, res->ai_addr, res->ai_addrlen);

	/* immediately successful connect */
	if(result == 0) {
		result = OK;
		/*printf("IMMEDIATE SUCCESS\n");*/
		}

	/* connection error */
	else if(result < 0 && errno != EINPROGRESS) {
		result = ERROR;
		}

	/* connection in progress - wait for it... */
	else {

		do {
			/* set connection timeout */
			tv.tv_sec = timeout;
			tv.tv_usec = 0;

			FD_ZERO(&wfds);
			FD_SET(*sd, &wfds);
			rfds = wfds;

			/* wait for readiness */
			result = select((*sd) + 1, &rfds, &wfds, NULL, &tv);

			/*printf("SELECT RESULT: %d\n",result);*/

			/* timeout */
			if(result == 0) {
				/*printf("TIMEOUT\n");*/
				result = ERROR;
				break;
				}

			/* an error occurred */
			if(result < 0 && errno != EINTR) {
				result = ERROR;
				break;
				}

			/* got something - check it */
			else if(result > 0) {

				/* get socket options to check for errors */
				optlen = sizeof(int);
				if(getsockopt(*sd, SOL_SOCKET, SO_ERROR, (void *)(&optval), &optlen) < 0) {
					result = ERROR;
					break;
					}

				/* an error occurred in the connection */
				if(optval != 0) {
					result = ERROR;
					break;
					}

				/* the connection was good! */
				/*
				printf("CONNECT SELECT: ERRNO=%s\n",strerror(errno));
				printf("CONNECT SELECT: OPTVAL=%s\n",strerror(optval));
				*/
				result = OK;
				break;
				}

			/* some other error occurred */
			else {
				result = ERROR;
				break;
				}

			}
		while(1);
		}


	freeaddrinfo(res);

	SSL_library_init();
	SSL_load_error_strings();

#if OPENSSL_VERSION_NUMBER >= 0x10100000

	method = TLS_method();

#else		/* OPENSSL_VERSION_NUMBER >= 0x10100000 */

# ifdef SSL_TXT_TLSV1_3
	method = TLSv1_3_client_method();
# else
#  ifdef SSL_TXT_TLSV1_2
	method = TLSv1_2_client_method();
#  else
#   ifdef SSL_TXT_TLSV1_1
	method = TLSv1_1_client_method();
#   else
	method = TLSv1_client_method();
#   endif
#  endif
# endif
#endif

	*ctx = SSL_CTX_new(method);
	*ssl = SSL_new(*ctx);
	BIO *bio = BIO_new_socket(*sd, BIO_NOCLOSE);
	SSL_set_bio(*ssl, bio, bio);
    SSL_CTX_set_verify(*ctx, SSL_VERIFY_PEER, NULL);
    if(SSL_CTX_set_default_verify_paths(*ctx) == 0) {
    	return ERROR;
    }

	do {

		/* set connection timeout */
		tv.tv_sec = timeout;
		tv.tv_usec = 0;

		FD_ZERO(&wfds);
		FD_SET(*sd, &wfds);
		rfds = wfds;

		/* wait for readiness */
		result = select((*sd) + 1, &rfds, &wfds, NULL, &tv);
		if (result == 0) {
			/* timeout */
			result = ERROR;
			break;
		}

		ssl_result = SSL_connect(*ssl);
		if (ssl_result <= 0) {


			int error = SSL_get_error(*ssl, ssl_result);
			if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
				/* An actual error happened */
				/* If we hit one of these two errors, we just want to select() the socket again */
				result = ERROR;
				break;
			}
		}
		result = OK;
	} while (ssl_result <= 0);

	return result;
}

int my_ssl_sendall(int s, SSL *ssl, const char *buf, int *len, int timeout) {
	int total_sent = 0;
	int bytes_left = 0;
	int n;
	fd_set wfds;
	struct timeval tv;
	int result = OK;
	time_t start_time;
	time_t current_time;

	time(&start_time);

	bytes_left = *len;
	while(total_sent < *len) {

		/* set send timeout */
		tv.tv_sec = timeout;
		tv.tv_usec = 0;

		FD_ZERO(&wfds);
		FD_SET(s, &wfds);

		/* wait for readiness */
		result = select(s + 1, NULL, &wfds, NULL, &tv);

		/* timeout */
		if(result == 0) {
			/*printf("RECV SELECT TIMEOUT\n");*/
			result = ERROR;
			break;
			}
		/* error */
		else if(result < 0) {
			/*printf("RECV SELECT ERROR: %s\n",strerror(errno));*/
			result = ERROR;
			break;
			}

		/* we're ready to write some data */
		result = OK;

		/* send the data */
		n = SSL_write(ssl, buf + total_sent, bytes_left);
		if(n <= 0) {
			int error = SSL_get_error(ssl, n);
			if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
				/* An actual error happened */
				/* If we hit one of these two errors, we just want to select() the socket again */
				break;
			}
		}

		total_sent += n;
		bytes_left -= n;

		/* make sure we haven't overrun the timeout */
		time(&current_time);
		if(current_time - start_time > timeout) {
			result = ERROR;
			break;
		}
	}

	*len = total_sent;

	return result;
	
}


/* receives all data in non-blocking mode with a timeout  - modelled after sendall() */
int my_ssl_recvall(int s, SSL *ssl, char *buf, int *len, int timeout) {
	int total_received = 0;
	int bytes_left = *len;
	int n = 0;
	time_t start_time;
	time_t current_time;
	fd_set rfds;
	struct timeval tv;
	int result = OK;

	/* clear the receive buffer */
	bzero(buf, *len);

	time(&start_time);

	/* receive all data */
	while(total_received < *len) {

		/* set receive timeout */
		tv.tv_sec = timeout;
		tv.tv_usec = 0;

		FD_ZERO(&rfds);
		FD_SET(s, &rfds);

		/* wait for readiness */
		result = select(s + 1, &rfds, NULL, NULL, &tv);

		/* timeout */
		if(result == 0) {
			/*printf("RECV SELECT TIMEOUT\n");*/
			result = ERROR;
			break;
			}
		/* error */
		else if(result < 0) {
			/*printf("RECV SELECT ERROR: %s\n",strerror(errno));*/
			result = ERROR;
			break;
			}

		/* we're ready to read some data */
		result = OK;

		/* receive some data */
		n = SSL_read(ssl, buf + total_received, bytes_left);
		if(n <= 0) {
			int error = SSL_get_error(ssl, n);
			if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
				/* An actual error happened */
				/* If we hit one of these two errors, we just want to select() the socket again */
				break;
			}
		}

		/* apply bytes we received */
		total_received += n;
		bytes_left -= n;

		/* make sure we haven't overrun the timeout */
		time(&current_time);
		if(current_time - start_time > timeout) {
			result = ERROR;
			break;
			}
		}

	/* return number of bytes actually received here */
	*len = total_received;

	return result;
	}


#endif

/* connect to a TCP socket in nonblocking fashion */
int my_tcp_connect(const char *host_name, int port, int *sd, int timeout) {
	struct addrinfo hints;
	struct addrinfo *res;
	int result;
	char port_str[6];
	int flags = 0;
	fd_set rfds;
	fd_set wfds;
	struct timeval tv;
	int optval;
	socklen_t optlen;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_INET;
	hints.ai_socktype = SOCK_STREAM;

	/* make sure our static port_str is long enough */
	if(port > 65535)
		return ERROR;

	snprintf(port_str, sizeof(port_str), "%d", port);
	result = getaddrinfo(host_name, port_str, &hints, &res);
	if(result != 0) {
		/*printf("GETADDRINFO: %s (%s) = %s\n",host_name,port_str,gai_strerror(result));*/
		return ERROR;
		}

	/* create a socket */
	*sd = socket(res->ai_family, SOCK_STREAM, res->ai_protocol);
	if(*sd < 0) {
		freeaddrinfo(res);
		return ERROR;
		}

	/* make socket non-blocking */
	flags = fcntl(*sd, F_GETFL, 0);
	fcntl(*sd, F_SETFL, flags | O_NONBLOCK);

	/* attempt to connect */
	result = connect(*sd, res->ai_addr, res->ai_addrlen);

	/* immediately successful connect */
	if(result == 0) {
		result = OK;
		/*printf("IMMEDIATE SUCCESS\n");*/
		}

	/* connection error */
	else if(result < 0 && errno != EINPROGRESS) {
		result = ERROR;
		}

	/* connection in progress - wait for it... */
	else {

		do {
			/* set connection timeout */
			tv.tv_sec = timeout;
			tv.tv_usec = 0;

			FD_ZERO(&wfds);
			FD_SET(*sd, &wfds);
			rfds = wfds;

			/* wait for readiness */
			result = select((*sd) + 1, &rfds, &wfds, NULL, &tv);

			/*printf("SELECT RESULT: %d\n",result);*/

			/* timeout */
			if(result == 0) {
				/*printf("TIMEOUT\n");*/
				result = ERROR;
				break;
				}
			else if(result < 0) {
				/*printf("RECV SELECT ERROR: %s\n",strerror(errno));*/
				result = ERROR;
				break;
				}

			/* got something - check it */
			else if(result > 0) {

				/* get socket options to check for errors */
				optlen = sizeof(int);
				if(getsockopt(*sd, SOL_SOCKET, SO_ERROR, (void *)(&optval), &optlen) < 0) {
					result = ERROR;
					break;
					}

				/* an error occurred in the connection */
				if(optval != 0) {
					result = ERROR;
					break;
					}

				/* the connection was good! */
				/*
				printf("CONNECT SELECT: ERRNO=%s\n",strerror(errno));
				printf("CONNECT SELECT: OPTVAL=%s\n",strerror(optval));
				*/
				result = OK;
				break;
				}

			/* some other error occurred */
			else {
				result = ERROR;
				break;
				}

			}
		while(1);
		}


	freeaddrinfo(res);

	return result;
	}


/* based on Beej's sendall - thanks Beej! */
int my_sendall(int s, const char *buf, int *len, int timeout) {
	int total_sent = 0;
	int bytes_left = 0;
	int n;
	fd_set wfds;
	struct timeval tv;
	int result = OK;
	time_t start_time;
	time_t current_time;

	time(&start_time);

	bytes_left = *len;
	while(total_sent < *len) {

		/* set send timeout */
		tv.tv_sec = timeout;
		tv.tv_usec = 0;

		FD_ZERO(&wfds);
		FD_SET(s, &wfds);

		/* wait for readiness */
		result = select(s + 1, NULL, &wfds, NULL, &tv);

		/* timeout */
		if(result == 0) {
			/*printf("RECV SELECT TIMEOUT\n");*/
			result = ERROR;
			break;
			}
		/* error */
		else if(result < 0) {
			/*printf("RECV SELECT ERROR: %s\n",strerror(errno));*/
			result = ERROR;
			break;
			}

		/* we're ready to write some data */
		result = OK;

		/* send the data */
		n = send(s, buf + total_sent, bytes_left, 0);
		if(n == -1) {
			/*printf("SEND ERROR: (%d) %s\n",s,strerror(errno));*/
			break;
			}

		total_sent += n;
		bytes_left -= n;

		/* make sure we haven't overrun the timeout */
		time(&current_time);
		if(current_time - start_time > timeout) {
			result = ERROR;
			break;
			}
		}

	*len = total_sent;

	return result;
	}


/* receives all data in non-blocking mode with a timeout  - modelled after sendall() */
int my_recvall(int s, char *buf, int *len, int timeout) {
	int total_received = 0;
	int bytes_left = *len;
	int n = 0;
	time_t start_time;
	time_t current_time;
	fd_set rfds;
	struct timeval tv;
	int result = OK;

	/* clear the receive buffer */
	bzero(buf, *len);

	time(&start_time);

	/* receive all data */
	while(total_received < *len) {

		/* set receive timeout */
		tv.tv_sec = timeout;
		tv.tv_usec = 0;

		FD_ZERO(&rfds);
		FD_SET(s, &rfds);

		/* wait for readiness */
		result = select(s + 1, &rfds, NULL, NULL, &tv);

		/* timeout */
		if(result == 0) {
			/*printf("RECV SELECT TIMEOUT\n");*/
			result = ERROR;
			break;
			}
		/* error */
		else if(result < 0) {
			/*printf("RECV SELECT ERROR: %s\n",strerror(errno));*/
			result = ERROR;
			break;
			}

		/* we're ready to read some data */
		result = OK;

		/* receive some data */
		n = recv(s, buf + total_received, bytes_left, 0);

		/* server disconnected */
		if(n == 0) {
			/*printf("SERVER DISCONNECT\n");*/
			break;
			}

		/* apply bytes we received */
		total_received += n;
		bytes_left -= n;

		/* make sure we haven't overrun the timeout */
		time(&current_time);
		if(current_time - start_time > timeout) {
			result = ERROR;
			break;
			}
		}

	/* return number of bytes actually received here */
	*len = total_received;

	return result;
	}