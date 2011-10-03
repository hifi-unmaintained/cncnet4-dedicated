/*
 * Copyright (c) 2011 Toni Spets <toni.spets@iki.fi>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "net.h"

/* mingw supports it and I really want getopt(3) */
#include <unistd.h>
#include <stdlib.h>

/* configurable trough parameters */
int port = 9000;
char ip[32] = { "0.0.0.0" };
char hostname[256] = { "Unnamed CnCNet Dedicated Server" };
char password[32] = { "" };
uint32_t timeout = 60;
uint8_t maxclients = 8;

uint8_t clients = 0;
time_t peer_last_packet[MAX_PEERS];
int32_t peer_whitelist[MAX_PEERS];
#define STAT_INTERVAL 5

void ann_main(const char *url);

int main(int argc, char **argv)
{
    int s,i;
    fd_set rfds;
    struct timeval tv;
    struct sockaddr_in peer;
    char buf[NET_BUF_SIZE];
    char status[256] = { 0 };

    uint32_t last_packets = 0;
    uint32_t last_bytes = 0;
    uint32_t last_time = 0;

    uint32_t total_packets = 0;
    uint32_t total_bytes = 0;
    uint32_t bps = 0;
    uint32_t pps = 0;

    int opt;
    while ((opt = getopt(argc, argv, "?hi:n:p:t:c:")) != -1)
    {
        switch (opt)
        {
            case 'i':
                strncpy(ip, optarg, sizeof(ip)-1);
                break;
            case 'n':
                strncpy(hostname, optarg, sizeof(hostname)-1);
                break;
            case 'p':
                strncpy(password, optarg, sizeof(password)-1);
                break;
            case 't':
                timeout = atoi(optarg);
                if (timeout < 1)
                {
                    timeout = 1;
                }
                else if (timeout > 3600)
                {
                    timeout = 3600;
                }
                break;
            case 'c':
                maxclients = atoi(optarg);
                if (maxclients < 2)
                {
                    maxclients = 2;
                }
                else if (maxclients > MAX_PEERS)
                {
                    maxclients = MAX_PEERS;
                }
                break;
            case 'h':
            case '?':
            default:
                fprintf(stderr, "Usage: %s [-h?] [-i ip] [-n hostname] [-p password] [-t timeout] [-c maxclients] [port]\n", argv[0]);
                return 1;
        }
    }

    if (optind < argc)
    {
        port = atoi(argv[optind]);
        if (port < 1024)
        {
            port = 1024;
        }
        else if (port > 65535)
        {
            port = 65535;
        }
    }

    s = net_init();

    printf("CnCNet Dedicated Server\n");
    printf("=======================\n");
    printf("         ip: %s\n", ip);
    printf("       port: %d\n", port);
    printf("   hostname: %s\n", hostname);
    printf("   password: %s\n", strlen(password) > 0 ? password : "<no password>");
    printf("    timeout: %d seconds\n", timeout);
    printf(" maxclients: %d\n", maxclients);
    printf("\n");

    net_bind(ip, port);

    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    memset(&tv, 0, sizeof(tv));
    memset(&peer_last_packet, 0, sizeof(peer_last_packet));
    memset(&peer_whitelist, 0, sizeof(peer_whitelist));

    while (select(s + 1, &rfds, NULL, NULL, &tv) > -1)
    {
        time_t now = time(NULL);

        /* clear old status line */
        printf("\r");
        for (i=0;i<strlen(status);i++)
            printf(" ");
        printf("\r");

        if (FD_ISSET(s, &rfds))
        {
            size_t len = net_recv(&peer);
            uint8_t cmd, peer_id;

            total_packets++;
            total_bytes += len;

            if (len == 0)
            {
                goto next;
            }

            cmd = net_read_int8();

            if (cmd == CMD_CONTROL)
            {
                uint8_t ctl = net_read_int8();

                /* reply with the same header, rest is control command specific */
                net_write_int8(cmd);
                net_write_int8(ctl);

                if (ctl == CTL_PING)
                {
                    /* ping is only used to test it the server is alive, so it contains no additional data */
                    net_send(&peer);
                }
                else if (ctl == CTL_QUERY)
                {
                    /* query responds with the basic server information to display on a server browser */
                    net_write_int8(strlen(password) > 0);
                    net_write_string(hostname);
                    net_write_int8(clients);
                    net_write_int8(maxclients);
                    net_send(&peer);
                }
                else if (ctl == CTL_RESET)
                {
                    /* reset sets a new whitelist if server has a password */
                    net_read_string(buf, sizeof(buf));

                    /* validate control password */
                    if (strlen(password) != 0 && strcmp(buf, password) == 0)
                    {
                        net_peer_reset();
                        memset(peer_last_packet, 0, sizeof(peer_last_packet));
                        memset(&peer_whitelist, 0, sizeof(peer_whitelist));

                        printf("Got %d ips trough CTL_RESET\n", net_read_size() / 4);

                        i = 0;
                        while (net_read_size() >= 4)
                        {
                            peer_whitelist[i] = net_read_int32();
                            printf(" %s\n", inet_ntoa(*(struct in_addr *)&peer_whitelist[i]));
                            i++;
                        }

                        net_write_int8(1);
                    }
                    else
                    {
                        printf("CTL_RESET with invalid password\n");
                        net_write_int8(0);
                    }

                    net_send(&peer);
                }
                else if (ctl == CTL_DISCONNECT)
                {
                    /* special packet from clients who are closing the socket so we can remove them from the active list before timeout */
                    peer_id = net_peer_get_by_addr(&peer);
                    if (peer_id != UINT8_MAX)
                    {
                        net_peer_remove(peer_id);
                        peer_last_packet[peer_id] = 0;
                    }
                }

                goto next;
            }

            peer_id = net_peer_get_by_addr(&peer);
            if (peer_id == UINT8_MAX && net_peer_count() < maxclients)
            {
                if (strlen(password) == 0)
                {
                    peer_id = net_peer_add(&peer);
                }
                else
                {
                    /* allow only whitelisted ips */
                    for (i = 0; i < MAX_PEERS; i++)
                    {
                        if (peer_whitelist[i] == peer.sin_addr.s_addr)
                        {
                            peer_id = net_peer_add(&peer);
                            break;
                        }
                    }
                }
            }

            if (peer_id == UINT8_MAX)
            {
                goto next;
            }

            peer_last_packet[peer_id] = now;

            len = net_read_data(buf, sizeof(buf));
            net_write_int8(peer_id);
            net_write_data(buf, len);

            if (cmd == CMD_BROADCAST)
            {
                net_broadcast(peer_id);
            }
            else if (cmd != peer_id)
            {
                struct sockaddr_in *to = net_peer_get(cmd);
                if (to)
                {
                    net_send(to);
                }
            }

        }

        clients = 0;
        for (i = 0; i < MAX_PEERS; i++)
        {
            if (peer_last_packet[i] > 0)
            {
                if (peer_last_packet[i] + timeout < now)
                {
                    net_peer_remove(i);
                    peer_last_packet[i] = 0;
                }
                else
                {
                    clients++;
                }
            }
        }

next:
        net_send_discard();
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        if (now >= last_time + STAT_INTERVAL)
        {
            int stat_elapsed = now - last_time ;
            pps = (total_packets - last_packets) / stat_elapsed;
            bps = (total_bytes - last_bytes) / stat_elapsed;
            last_packets = total_packets;
            last_bytes = total_bytes;
            last_time = now;
        }

        snprintf(status, sizeof(status)-1, "%s (%d/%d) [ %d p/s, %d kB/s | total: %d p, %d kB ]",
                hostname, clients, maxclients, pps, bps / 1024, total_packets, total_bytes / 1024);

        printf("%s", status);
        fflush(stdout);
    }

    net_free();
    return 0;
}
