/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002 Cray Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* connection.c:
   This file provides a simple stateful connection manager which
   builds tcp connections on demand and leaves them open for
   future use. It also provides the machinery to allow peers
   to connect to it
*/

#include <stdlib.h>
#include <pqtimer.h>
#include <dispatch.h>
#include <table.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <portals/types.h>
#include <portals/list.h>
#include <portals/lib-types.h>
#include <portals/socknal.h>
#include <linux/kp30.h>
#include <connection.h>
#include <pthread.h>
#include <errno.h>
#ifndef __CYGWIN__
#include <syscall.h>
#endif

/* global variable: acceptor port */
unsigned short tcpnal_acceptor_port = 988;


/* Function:  compare_connection
 * Arguments: connection c:      a connection in the hash table
 *            ptl_process_id_t:  an id to verify  agains
 * Returns: 1 if the connection is the one requested, 0 otherwise
 *
 *    compare_connection() tests for collisions in the hash table
 */
static int compare_connection(void *arg1, void *arg2)
{
    connection c = arg1;
    unsigned int * id = arg2;
#if 0
    return((c->ip==id[0]) && (c->port==id[1]));
#else
    /* CFS specific hacking */
    return (c->ip == id[0]);
#endif
}


/* Function:  connection_key
 * Arguments: ptl_process_id_t id:  an id to hash
 * Returns: a not-particularily-well-distributed hash
 *          of the id
 */
static unsigned int connection_key(unsigned int *id)
{
#if 0
    return(id[0]^id[1]);
#else
    /* CFS specific hacking */
    return (unsigned int) id[0];
#endif
}


/* Function:  remove_connection
 * Arguments: c: the connection to remove
 */
void remove_connection(void *arg)
{
        connection c = arg;
        unsigned int id[2];
        
        id[0]=c->ip;
        id[1]=c->port;
        hash_table_remove(c->m->connections,id);
        close(c->fd);
        free(c);
}


/* Function:  read_connection: 
 * Arguments: c:    the connection to read from 
 *            dest: the buffer to read into
 *            len:  the number of bytes to read   
 * Returns: success as 1, or failure as 0
 *
 *   read_connection() reads data from the connection, continuing
 *   to read partial results until the request is satisfied or
 *   it errors. TODO: this read should be covered by signal protection.
 */
int read_connection(connection c,
                    unsigned char *dest,
                    int len)
{
    int offset = 0,rc;

    if (len) {
        do {
#ifndef __CYGWIN__
            rc = syscall(SYS_read, c->fd, dest+offset, len-offset);
#else
            rc = recv(c->fd, dest+offset, len-offset, 0);
#endif
            if (rc <= 0) {
                if (errno == EINTR) {
                    rc = 0;
                } else {
                    remove_connection(c);
                    return (0);
                }
            }
            offset += rc;
        } while (offset < len);
    }
    return (1);
}

static int connection_input(void *d)
{
        connection c = d;
        return((*c->m->handler)(c->m->handler_arg,c));
}


/* Function:  allocate_connection
 * Arguments: t:    tcpnal the allocation is occuring in the context of
 *            dest: portal endpoint address for this connection
 *            fd:   open file descriptor for the socket
 * Returns: an allocated connection structure
 *
 * just encompasses the action common to active and passive
 *  connections of allocation and placement in the global table
 */
static connection allocate_connection(manager m,
                               unsigned int ip,
                               unsigned short port,
                               int fd)
{
    connection c=malloc(sizeof(struct connection));
    unsigned int id[2];
    c->m=m;
    c->fd=fd;
    c->ip=ip;
    c->port=port;
    id[0]=ip;
    id[1]=port;
    register_io_handler(fd,READ_HANDLER,connection_input,c);
    hash_table_insert(m->connections,c,id);
    return(c);
}


/* Function:  new_connection
 * Arguments: t: opaque argument holding the tcpname
 * Returns: 1 in order to reregister for new connection requests
 *
 *  called when the bound service socket recieves
 *     a new connection request, it always accepts and
 *     installs a new connection
 */
static int new_connection(void *z)
{
    manager m=z;
    struct sockaddr_in s;
    int len=sizeof(struct sockaddr_in);
    int fd=accept(m->bound,(struct sockaddr *)&s,&len);
    unsigned int nid=*((unsigned int *)&s.sin_addr);
    /* cfs specific hack */
    //unsigned short pid=s.sin_port;
    pthread_mutex_lock(&m->conn_lock);
    allocate_connection(m,htonl(nid),0/*pid*/,fd);
    pthread_mutex_unlock(&m->conn_lock);
    return(1);
}

/* FIXME assuming little endian, cleanup!! */
#define __cpu_to_le64(x) ((__u64)(x))
#define __le64_to_cpu(x) ((__u64)(x))
#define __cpu_to_le32(x) ((__u32)(x))
#define __le32_to_cpu(x) ((__u32)(x))
#define __cpu_to_le16(x) ((__u16)(x))
#define __le16_to_cpu(x) ((__u16)(x))

extern ptl_nid_t tcpnal_mynid;

int
tcpnal_hello (int sockfd, ptl_nid_t *nid, int type, __u64 incarnation)
{
        int                 rc;
        ptl_hdr_t           hdr;
        ptl_magicversion_t *hmv = (ptl_magicversion_t *)&hdr.dest_nid;

        LASSERT (sizeof (*hmv) == sizeof (hdr.dest_nid));

        memset (&hdr, 0, sizeof (hdr));
        hmv->magic         = __cpu_to_le32 (PORTALS_PROTO_MAGIC);
        hmv->version_major = __cpu_to_le32 (PORTALS_PROTO_VERSION_MAJOR);
        hmv->version_minor = __cpu_to_le32 (PORTALS_PROTO_VERSION_MINOR);
        
        hdr.src_nid = __cpu_to_le64 (tcpnal_mynid);
        hdr.type    = __cpu_to_le32 (PTL_MSG_HELLO);

        hdr.msg.hello.type = __cpu_to_le32 (type);
        hdr.msg.hello.incarnation = 0;

        /* Assume sufficient socket buffering for this message */
        rc = syscall(SYS_write, sockfd, &hdr, sizeof(hdr));
        if (rc <= 0) {
                CERROR ("Error %d sending HELLO to %llx\n", rc, *nid);
                return (rc);
        }

        rc = syscall(SYS_read, sockfd, hmv, sizeof(*hmv));
        if (rc <= 0) {
                CERROR ("Error %d reading HELLO from %llx\n", rc, *nid);
                return (rc);
        }
        
        if (hmv->magic != __le32_to_cpu (PORTALS_PROTO_MAGIC)) {
                CERROR ("Bad magic %#08x (%#08x expected) from %llx\n",
                        __cpu_to_le32 (hmv->magic), PORTALS_PROTO_MAGIC, *nid);
                return (-EPROTO);
        }

        if (hmv->version_major != __cpu_to_le16 (PORTALS_PROTO_VERSION_MAJOR) ||
            hmv->version_minor != __cpu_to_le16 (PORTALS_PROTO_VERSION_MINOR)) {
                CERROR ("Incompatible protocol version %d.%d (%d.%d expected)"
                        " from %llx\n",
                        __le16_to_cpu (hmv->version_major),
                        __le16_to_cpu (hmv->version_minor),
                        PORTALS_PROTO_VERSION_MAJOR,
                        PORTALS_PROTO_VERSION_MINOR,
                        *nid);
                return (-EPROTO);
        }

#if (PORTALS_PROTO_VERSION_MAJOR != 0)
# error "This code only understands protocol version 0.x"
#endif
        /* version 0 sends magic/version as the dest_nid of a 'hello' header,
         * so read the rest of it in now... */

        rc = syscall(SYS_read, sockfd, hmv + 1, sizeof(hdr) - sizeof(*hmv));
        if (rc <= 0) {
                CERROR ("Error %d reading rest of HELLO hdr from %llx\n",
                        rc, *nid);
                return (rc);
        }

        /* ...and check we got what we expected */
        if (hdr.type != __cpu_to_le32 (PTL_MSG_HELLO) ||
            hdr.payload_length != __cpu_to_le32 (0)) {
                CERROR ("Expecting a HELLO hdr with 0 payload,"
                        " but got type %d with %d payload from %llx\n",
                        __le32_to_cpu (hdr.type),
                        __le32_to_cpu (hdr.payload_length), *nid);
                return (-EPROTO);
        }

        if (__le64_to_cpu(hdr.src_nid) == PTL_NID_ANY) {
                CERROR("Expecting a HELLO hdr with a NID, but got PTL_NID_ANY\n");
                return (-EPROTO);
        }

        if (*nid == PTL_NID_ANY) {              /* don't know peer's nid yet */
                *nid = __le64_to_cpu(hdr.src_nid);
        } else if (*nid != __le64_to_cpu (hdr.src_nid)) {
                CERROR ("Connected to nid %llx, but expecting %llx\n",
                        __le64_to_cpu (hdr.src_nid), *nid);
                return (-EPROTO);
        }

        return (0);
}

/* Function:  force_tcp_connection
 * Arguments: t: tcpnal
 *            dest: portals endpoint for the connection
 * Returns: an allocated connection structure, either
 *          a pre-existing one, or a new connection
 */
connection force_tcp_connection(manager m,
                                unsigned int ip,
                                unsigned short port,
                                procbridge pb)
{
    connection conn;
    struct sockaddr_in addr;
    unsigned int id[2];

    port = tcpnal_acceptor_port;

    id[0] = ip;
    id[1] = port;

    pthread_mutex_lock(&m->conn_lock);

    conn = hash_table_find(m->connections, id);
    if (!conn) {
        int fd;
        int option;
        ptl_nid_t peernid = PTL_NID_ANY;

        bzero((char *) &addr, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(ip);
        addr.sin_port        = htons(port);

        if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { 
            perror("tcpnal socket failed");
            exit(-1);
        }
        if (connect(fd, (struct sockaddr *)&addr,
                    sizeof(struct sockaddr_in))) {
            perror("tcpnal connect");
            return(0);
        }

#if 1
        option = 1;
        setsockopt(fd, SOL_TCP, TCP_NODELAY, &option, sizeof(option));
        option = 1<<20;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &option, sizeof(option));
        option = 1<<20;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &option, sizeof(option));
#endif
   
        /* say hello */
        if (tcpnal_hello(fd, &peernid, SOCKNAL_CONN_ANY, 0))
            exit(-1);

        conn = allocate_connection(m, ip, port, fd);

        /* let nal thread know this event right away */
        if (conn)
                procbridge_wakeup_nal(pb);
    }

    pthread_mutex_unlock(&m->conn_lock);
    return (conn);
}


/* Function:  bind_socket
 * Arguments: t: the nal state for this interface
 *            port: the port to attempt to bind to
 * Returns: 1 on success, or 0 on error
 *
 * bind_socket() attempts to allocate and bind a socket to the requested
 *  port, or dynamically assign one from the kernel should the port be
 *  zero. Sets the bound and bound_handler elements of m.
 *
 *  TODO: The port should be an explicitly sized type.
 */
static int bind_socket(manager m,unsigned short port)
{
    struct sockaddr_in addr;
    int alen=sizeof(struct sockaddr_in);
    
    if ((m->bound = socket(AF_INET, SOCK_STREAM, 0)) < 0)  
        return(0);
    
    bzero((char *) &addr, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = 0;
    addr.sin_port        = htons(port);

    if (bind(m->bound,(struct sockaddr *)&addr,alen)<0){
        perror ("tcpnal bind"); 
        return(0);
    }
    
    getsockname(m->bound,(struct sockaddr *)&addr, &alen);

    m->bound_handler=register_io_handler(m->bound,READ_HANDLER,
                                         new_connection,m);
    listen(m->bound,5); 
    m->port=addr.sin_port;
    return(1);
}


/* Function:  shutdown_connections
 * Arguments: m: the manager structure
 *
 * close all connections and reclaim resources
 */
void shutdown_connections(manager m)
{
    close(m->bound);
    remove_io_handler(m->bound_handler);
    hash_destroy_table(m->connections,remove_connection);
    free(m);
}


/* Function:  init_connections
 * Arguments: t: the nal state for this interface
 *            port: the port to attempt to bind to
 * Returns: a newly allocated manager structure, or
 *          zero if the fixed port could not be bound
 */
manager init_connections(unsigned short pid,
                         int (*input)(void *, void *),
                         void *a)
{
    manager m = (manager)malloc(sizeof(struct manager));
    m->connections = hash_create_table(compare_connection,connection_key);
    m->handler = input;
    m->handler_arg = a;
    pthread_mutex_init(&m->conn_lock, 0);

    if (bind_socket(m,pid))
        return(m);

    free(m);
    return(0);
}
