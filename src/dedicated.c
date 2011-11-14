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
#include <signal.h>
#include "net.h"
#include "log.h"

/* mingw supports it and I really want getopt(3) */
#include <unistd.h>
#include <stdlib.h>

enum
{
    GAME_UNKNOWN,
    GAME_CNC95,
    GAME_RA95,
    GAME_TS,
    GAME_TSDTA,
    GAME_RA2,
    GAME_LAST
};

typedef struct
{
    uint8_t game;
    uint8_t link_id;
} client_data;

/* configurable trough parameters */
int port = 9000;
char ip[32] = { "0.0.0.0" };
char linkto[256] = { "" };
char hostname[256] = { "Unnamed CnCNet Dedicated Server" };
char password[32] = { "" };
uint32_t timeout = 60;
uint8_t maxclients = 8;

uint8_t clients = 0;
time_t peer_last_packet[MAX_PEERS];
int32_t peer_whitelist[MAX_PEERS];

void ann_main(const char *url);

int interrupt = 0;
void onsigint(int signum)
{
    log_status_clear();
    printf("Received ^C, exiting...");
    interrupt = 1;
}
void onsigterm(int signum)
{
    log_status_clear();
    printf("Received SIGTERM, exiting...");
    interrupt = 1;
}

int main(int argc, char **argv)
{
    int s,i;
    fd_set rfds;
    struct timeval tv;
    struct sockaddr_in peer;
    char buf[NET_BUF_SIZE];
    time_t booted = time(NULL);

    uint32_t last_packets = 0;
    uint32_t last_bytes = 0;
    uint32_t last_time = 0;

    uint32_t total_packets = 0;
    uint32_t total_bytes = 0;
    uint32_t bps = 0;
    uint32_t pps = 0;

    int opt;
    while ((opt = getopt(argc, argv, "?hi:n:p:t:c:l:")) != -1)
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
            case 'l':
                strncpy(linkto, optarg, sizeof(linkto)-1);
                break;
            case 'h':
            case '?':
            default:
                fprintf(stderr, "Usage: %s [-h?] [-i ip] [-n hostname] [-p password] [-t timeout] [-c maxclients] [-l server[:port]] [port]\n", argv[0]);
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
    printf("     linkto: %s\n", strlen(linkto) > 0 ? linkto : "<none>");
    printf("   password: %s\n", strlen(password) > 0 ? password : "<no password>");
    printf("    timeout: %d seconds\n", timeout);
    printf(" maxclients: %d\n", maxclients);
    printf("    version: %s\n", VERSION);
    printf("\n");

    struct sockaddr_in link_addr;
    if (strlen(linkto))
    {
        int link_port = 9000;
        char *str_port = strstr(linkto, ":");
        if (str_port)
        {
            *str_port = '\0';
            link_port = atoi(++str_port);
        }

        net_address(&link_addr, linkto, link_port);
        printf("Linking to %s:%d\n\n", inet_ntoa(link_addr.sin_addr), ntohs(link_addr.sin_port));
    }

    net_bind(ip, port);

    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    memset(&tv, 0, sizeof(tv));
    memset(&peer_last_packet, 0, sizeof(peer_last_packet));
    memset(&peer_whitelist, 0, sizeof(peer_whitelist));

    signal(SIGINT, onsigint);
    signal(SIGTERM, onsigterm);

    while (select(s + 1, &rfds, NULL, NULL, &tv) > -1 && !interrupt)
    {
        time_t now = time(NULL);
        int from_proxy = 0;

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
                    goto next;
                }
                else if (ctl == CTL_QUERY)
                {
                    /* query responds with the basic server information to display on a server browser */
                    int i, cnt[GAME_LAST] = { 0, 0, 0, 0, 0 };
                    for (i = 0; i < MAX_PEERS; i++)
                    {
                        if (peer_last_packet[i])
                        {
                            client_data *cd = (client_data *)*net_peer_data(i);
                            if (cd)
                            {
                                cnt[cd->game]++;
                            }
                        }
                    }

                    net_write_string("hostname");
                    net_write_string(hostname);
                    net_write_string("password");
                    net_write_string_int32(strlen(password) > 0);
                    net_write_string("clients");
                    net_write_string_int32(clients);
                    net_write_string("maxclients");
                    net_write_string_int32(maxclients);
                    net_write_string("version");
                    net_write_string(VERSION);
                    net_write_string("uptime");
                    net_write_string_int32(now - booted);
                    net_write_string("unk");
                    net_write_string_int32(cnt[GAME_UNKNOWN]);
                    net_write_string("cnc95");
                    net_write_string_int32(cnt[GAME_CNC95]);
                    net_write_string("ra95");
                    net_write_string_int32(cnt[GAME_RA95]);
                    net_write_string("ts");
                    net_write_string_int32(cnt[GAME_TS]);
                    net_write_string("tsdta");
                    net_write_string_int32(cnt[GAME_TSDTA]);
                    net_write_string("ra2");
                    net_write_string_int32(cnt[GAME_RA2]);

                    net_send(&peer);
                    goto next;
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

                        log_printf("Got %d ips trough CTL_RESET\n", net_read_size() / 4);

                        i = 0;
                        while (net_read_size() >= 4)
                        {
                            peer_whitelist[i] = net_read_int32();
                            log_printf(" %s\n", inet_ntoa(*(struct in_addr *)&peer_whitelist[i]));
                            i++;
                        }

                        net_write_int8(1);
                    }
                    else
                    {
                        log_printf("CTL_RESET with invalid password\n");
                        net_write_int8(0);
                    }

                    net_send(&peer);
                    goto next;
                }
                else if (ctl == CTL_DISCONNECT)
                {
                    /* special packet from clients who are closing the socket so we can remove them from the active list before timeout */
                    peer_id = net_peer_get_by_addr(&peer);
                    if (peer_id != UINT8_MAX)
                    {
                        if (strlen(linkto) > 0)
                        {
                            net_write_int8(CMD_CONTROL);
                            net_write_int8(CTL_PROXY_DISCONNECT);
                            net_write_int8(peer_id);
                            net_send(&link_addr);
                        }
                        net_peer_remove(peer_id);
                        peer_last_packet[peer_id] = 0;
                        client_data *cd = (client_data *)*net_peer_data(peer_id);
                        if (cd)
                        {
                            cd->game = GAME_UNKNOWN;
                            cd->link_id = UINT8_MAX;
                        }
                    }
                    goto next;
                }
                else if (ctl == CTL_PROXY)
                {
                    int i;
                    int proxy_link_id = net_read_int8();
                    int proxy_cmd = net_read_int8();

                    if (peer.sin_addr.s_addr != link_addr.sin_addr.s_addr)
                    {
                        log_printf("Ignored proxy packet from invalid host.\n");
                        goto next;
                    }

                    /* try to find our remote client from local list */
                    peer_id = UINT8_MAX;
                    for (i = 0; i < MAX_PEERS; i++)
                    {
                        client_data *cd = (client_data *)*net_peer_data(i);
                        if (cd && cd->link_id == proxy_link_id)
                        {
                            peer_id = i;
                            break;
                        }
                    }

                    /* add to local list if not */
                    if (peer_id == UINT8_MAX && net_peer_count() < maxclients)
                    {
                        if (strlen(password) == 0)
                        {
                            peer_id = net_peer_add(&link_addr);
                        }
                        else
                        {
                            /* allow only whitelisted ips */
                            for (i = 0; i < MAX_PEERS; i++)
                            {
                                if (peer_whitelist[i] == peer.sin_addr.s_addr)
                                {
                                    peer_id = net_peer_add(&link_addr);
                                    break;
                                }
                            }
                        }

                        client_data *cd = (client_data *)*net_peer_data(peer_id);
                        if (cd == NULL)
                        {
                            cd = malloc(sizeof(client_data));
                            memset(cd, 0, sizeof(client_data));
                            *net_peer_data(peer_id) = (intptr_t)cd;
                        }

                        cd->link_id = proxy_link_id;
                    }

                    net_send_discard(); // flush out the default reply header

                    if (peer_id != UINT8_MAX)
                    {
                        cmd = proxy_cmd;
                        from_proxy = 1;
                        goto proxy_skip;
                    }
                    else
                    {
                        log_printf("Server full, proxy client rejected.\n");
                        goto next;
                    }
                }
                else if (ctl == CTL_PROXY_DISCONNECT)
                {
                    int i;
                    int proxy_link_id = net_read_int8();

                    if (peer.sin_addr.s_addr != link_addr.sin_addr.s_addr)
                    {
                        log_printf("Ignored proxy packet from invalid host.\n");
                        goto next;
                    }

                    /* try to find our remote client from local list */
                    peer_id = UINT8_MAX;
                    for (i = 0; i < MAX_PEERS; i++)
                    {
                        client_data *cd = (client_data *)*net_peer_data(i);
                        if (cd && cd->link_id == proxy_link_id)
                        {
                            net_peer_remove(i);
                            peer_last_packet[i] = 0;
                            client_data *cd = (client_data *)*net_peer_data(peer_id);
                            if (cd)
                            {
                                cd->game = GAME_UNKNOWN;
                                cd->link_id = UINT8_MAX;
                            }
                            break;
                        }
                    }

                    goto next;
                }
                else
                {
                    goto next;
                }
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

            proxy_skip: /* sorry for using goto, but I didn't care to refactor the code */

            peer_last_packet[peer_id] = now;
            client_data *cd = (client_data *)*net_peer_data(peer_id);
            if (cd == NULL)
            {
                cd = malloc(sizeof(client_data));
                memset(cd, 0, sizeof(client_data));
                cd->link_id = UINT8_MAX;
                *net_peer_data(peer_id) = (intptr_t)cd;
            }

            len = net_read_data(buf, sizeof(buf));

            if (cmd == CMD_BROADCAST)
            {
                net_write_int8(peer_id);
                net_write_data(buf, len);

                /* try to detect any supported game */
                if (buf[0] == 0x34 && buf[1] == 0x12)
                {
                    cd->game = GAME_CNC95;
                }
                else if (buf[0] == 0x35 && buf[1] == 0x12)
                {
                    cd->game = GAME_RA95;
                }
                else if (buf[4] == 0x35 && buf[5] == 0x12)
                {
                    cd->game = GAME_TS;
                }
                else if (buf[4] == 0x35 && buf[5] == 0x13)
                {
                    cd->game = GAME_TSDTA;
                }
                else if (buf[4] == 0x36 && buf[5] == 0x12)
                {
                    cd->game = GAME_RA2;
                }
                else
                {
                    cd->game = GAME_UNKNOWN;
                }

                int i;
                for (i = 0; i < MAX_PEERS; i++)
                {
                    client_data *cd = (client_data *)*net_peer_data(i);
                    if (i != peer_id && cd && cd->link_id == UINT8_MAX)
                    {
                        net_send_noflush(net_peer_get(i));
                    }
                }
                net_send_discard();

                /* broadcast to linked server */
                if (!from_proxy && strlen(linkto) > 0)
                {
                    net_write_int8(CMD_CONTROL);
                    net_write_int8(CTL_PROXY);
                    net_write_int8(peer_id);
                    net_write_int8(CMD_BROADCAST);
                    net_write_data(buf, len);
                    net_send(&link_addr);
                }
            }
            else if (cmd != peer_id)
            {
                client_data *target_cd = (client_data *)*net_peer_data(cmd);

                if (target_cd && target_cd->link_id != UINT8_MAX)
                {
                    net_write_int8(CMD_CONTROL);
                    net_write_int8(CTL_PROXY);
                    net_write_int8(peer_id);
                    net_write_int8(target_cd->link_id);
                    net_write_data(buf, len);
                    net_send(&link_addr);
                }
                else
                {
                    struct sockaddr_in *to = net_peer_get(cmd);
                    if (to)
                    {
                        net_write_int8(peer_id);
                        net_write_data(buf, len);
                        net_send(to);
                    }
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
                    client_data *cd = (client_data *)*net_peer_data(i);
                    if (cd)
                    {
                        cd->game = GAME_UNKNOWN;
                        cd->link_id = UINT8_MAX;
                    }
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

        if (now > last_time && !interrupt)
        {
            int stat_elapsed = now - last_time ;
            pps = (total_packets - last_packets) / stat_elapsed;
            bps = (total_bytes - last_bytes) / stat_elapsed;
            last_packets = total_packets;
            last_bytes = total_bytes;
            last_time = now;

            log_statusf("%s (%d/%d) [ %d p/s, %d kB/s | total: %d p, %d kB ]",
                hostname, clients, maxclients, pps, bps / 1024, total_packets, total_bytes / 1024);
        }
    }

    printf("\n");

    net_free();
    return 0;
}
