/*
 * Copyright (c) 2019 dsafa22, All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "raop_rtp_mirror.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>

#include "raop.h"
#include "netutils.h"
#include "compat.h"
#include "logger.h"
#include "byteutils.h"
#include "mirror_buffer.h"
#include "stream.h"


struct h264codec_s {
    unsigned char compatibility;
    short length_of_pps;
    short length_of_sps;
    unsigned char level;
    unsigned char number_of_pps;
    unsigned char* picture_parameter_set;
    unsigned char profile_high;
    unsigned char reserved_3_and_sps;
    unsigned char reserved_6_and_nal;
    unsigned char* sequence;
    unsigned char version;
};

struct raop_rtp_mirror_s {
    logger_t *logger;
    raop_callbacks_t callbacks;
    raop_ntp_t *ntp;

    /* Buffer to handle all resends */
    mirror_buffer_t *buffer;

    /* Remote address as sockaddr */
    struct sockaddr_storage remote_saddr;
    socklen_t remote_saddr_len;

    /* MUTEX LOCKED VARIABLES START */
    /* These variables only edited mutex locked */
    int running;
    int joined;

    int flush;
    thread_handle_t thread_mirror;
    mutex_handle_t run_mutex;

    /* MUTEX LOCKED VARIABLES END */
    int mirror_data_sock;

    unsigned short mirror_data_lport;
};

static int
raop_rtp_parse_remote(raop_rtp_mirror_t *raop_rtp_mirror, const unsigned char *remote, int remotelen)
{
    char current[25];
    int family;
    int ret;
    assert(raop_rtp_mirror);
    if (remotelen == 4) {
        family = AF_INET;
    } else if (remotelen == 16) {
        family = AF_INET6;
    } else {
        return -1;
    }
    memset(current, 0, sizeof(current));
    sprintf(current, "%d.%d.%d.%d", remote[0], remote[1], remote[2], remote[3]);
    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_parse_remote ip = %s", current);
    ret = netutils_parse_address(family, current,
                                 &raop_rtp_mirror->remote_saddr,
                                 sizeof(raop_rtp_mirror->remote_saddr));
    if (ret < 0) {
        return -1;
    }
    raop_rtp_mirror->remote_saddr_len = ret;
    return 0;
}

#define NO_FLUSH (-42)
raop_rtp_mirror_t *raop_rtp_mirror_init(logger_t *logger, raop_callbacks_t *callbacks, raop_ntp_t *ntp,
                                        const unsigned char *remote, int remotelen,
                                        const unsigned char *aeskey, const unsigned char *ecdh_secret)
{
    raop_rtp_mirror_t *raop_rtp_mirror;

    assert(logger);
    assert(callbacks);

    raop_rtp_mirror = calloc(1, sizeof(raop_rtp_mirror_t));
    if (!raop_rtp_mirror) {
        return NULL;
    }
    raop_rtp_mirror->logger = logger;
    raop_rtp_mirror->ntp = ntp;

    memcpy(&raop_rtp_mirror->callbacks, callbacks, sizeof(raop_callbacks_t));
    raop_rtp_mirror->buffer = mirror_buffer_init(logger, aeskey, ecdh_secret);
    if (!raop_rtp_mirror->buffer) {
        free(raop_rtp_mirror);
        return NULL;
    }
    if (raop_rtp_parse_remote(raop_rtp_mirror, remote, remotelen) < 0) {
        free(raop_rtp_mirror);
        return NULL;
    }
    raop_rtp_mirror->running = 0;
    raop_rtp_mirror->joined = 1;
    raop_rtp_mirror->flush = NO_FLUSH;

    MUTEX_CREATE(raop_rtp_mirror->run_mutex);
    return raop_rtp_mirror;
}

void
raop_rtp_init_mirror_aes(raop_rtp_mirror_t *raop_rtp_mirror, uint64_t streamConnectionID)
{
    mirror_buffer_init_aes(raop_rtp_mirror->buffer, streamConnectionID);
}

//#define DUMP_H264

#define RAOP_PACKET_LEN 32768
/**
 * Mirror
 */
static THREAD_RETVAL
raop_rtp_mirror_thread(void *arg)
{
    raop_rtp_mirror_t *raop_rtp_mirror = arg;
    int stream_fd = -1;
    unsigned char packet[128];
    memset(packet, 0 , 128);
    unsigned int readstart = 0;
    assert(raop_rtp_mirror);

#ifdef DUMP_H264
    // C decrypted
    FILE* file = fopen("/home/pi/Airplay.h264", "wb");
    // Encrypted source file
    FILE* file_source = fopen("/home/pi/Airplay.source", "wb");
    FILE* file_len = fopen("/home/pi/Airplay.len", "wb");
#endif
    while (1) {
        fd_set rfds;
        struct timeval tv;
        int nfds, ret;
        MUTEX_LOCK(raop_rtp_mirror->run_mutex);
        if (!raop_rtp_mirror->running) {
            MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
            break;
        }
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        /* Set timeout value to 5ms */
        tv.tv_sec = 0;
        tv.tv_usec = 5000;

        /* Get the correct nfds value and set rfds */
        FD_ZERO(&rfds);
        if (stream_fd == -1) {
            FD_SET(raop_rtp_mirror->mirror_data_sock, &rfds);
            nfds = raop_rtp_mirror->mirror_data_sock+1;
        } else {
            FD_SET(stream_fd, &rfds);
            nfds = stream_fd+1;
        }
        ret = select(nfds, &rfds, NULL, NULL, &tv);
        if (ret == 0) {
            /* Timeout happened */
            continue;
        } else if (ret == -1) {
            /* FIXME: Error happened */
            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Error in select");
            break;
        }
        if (stream_fd == -1 && FD_ISSET(raop_rtp_mirror->mirror_data_sock, &rfds)) {
            struct sockaddr_storage saddr;
            socklen_t saddrlen;

            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Accepting client");
            saddrlen = sizeof(saddr);
            stream_fd = accept(raop_rtp_mirror->mirror_data_sock, (struct sockaddr *)&saddr, &saddrlen);
            if (stream_fd == -1) {
                /* FIXME: Error happened */
                logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Error in accept %d %s", errno, strerror(errno));
                break;
            }
        }
        if (stream_fd != -1 && FD_ISSET(stream_fd, &rfds)) {
            // Packetlen initial 0
            ret = recv(stream_fd, packet + readstart, 4 - readstart, 0);
            if (ret == 0) {
                /* TCP socket closed */
                logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "TCP socket closed");
                break;
            } else if (ret == -1) {
                /* FIXME: Error happened */
                logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Error in recv");
                break;
            }
            readstart += ret;
            if (readstart < 4) {
                continue;
            }
            if ((packet[0] == 80 && packet[1] == 79 && packet[2] == 83 && packet[3] == 84) || (packet[0] == 71 && packet[1] == 69 && packet[2] == 84)) {
                // POST or GET
                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "handle http data");
            } else {
                // Common data block
                do {
                    // Read the remaining 124 bytes
                    ret = recv(stream_fd, packet + readstart, 128 - readstart, 0);
                    readstart = readstart + ret;
                } while (readstart < 128);
                int payloadsize = byteutils_get_int(packet, 0);
                // FIXME: The calculation method here needs to be confirmed again.
                short payloadtype = (short) (byteutils_get_short(packet, 4) & 0xff);
                short payloadoption = byteutils_get_short(packet, 6);

                // Processing content data
                if (payloadtype == 0) {
                    uint64_t ntp_timestamp_raw = byteutils_get_long(packet, 8);
                    // Conveniently, the video data is already stamped with the remote wall clock time,
                    // so no additional clock syncing needed. The only thing odd here is that the video
                    // ntp time stamps don't include the SECONDS_FROM_1900_TO_1970, so it's really just
                    // counting micro seconds since last boot.
                    uint64_t ntp_timestamp = raop_ntp_timestamp_to_micro_seconds(ntp_timestamp_raw, false);

                    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "video ntp = %llu", ntp_timestamp);

                    // Here is the encrypted data
                    unsigned char* payload_in = malloc(payloadsize);
                    unsigned char* payload = malloc(payloadsize);
                    readstart = 0;
                    do {
                        // Payload data
                        ret = recv(stream_fd, payload_in + readstart, payloadsize - readstart, 0);
                        readstart = readstart + ret;
                    } while (readstart < payloadsize);
                    //logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "readstart = %d", readstart);
#ifdef DUMP_H264
                    fwrite(payload_in, payloadsize, 1, file_source);
                    fwrite(&readstart, sizeof(readstart), 1, file_len);
#endif
                    
                    // Decrypt data
                    mirror_buffer_decrypt(raop_rtp_mirror->buffer, payload_in, payload, payloadsize);
                    int nalu_size = 0;
                    int nalu_num = 0;
                    while (nalu_size < payloadsize) {
                        int nc_len = (payload[nalu_size + 0] << 24) | (payload[nalu_size + 1] << 16) | (payload[nalu_size + 2] << 8) | (payload[nalu_size + 3]);
                        if (nc_len > 0) {
                            payload[nalu_size + 0] = 0;
                            payload[nalu_size + 1] = 0;
                            payload[nalu_size + 2] = 0;
                            payload[nalu_size + 3] = 1;
                            //int nalutype = payload[4] & 0x1f;
                            //logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "nalutype = %d", nalutype);
                            nalu_size += nc_len + 4;
                            nalu_num++;
                        }
                    }
                    //logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "nalu_size = %d, payloadsize = %d nalu_num = %d", nalu_size, payloadsize, nalu_num);

                    // Write file
#ifdef DUMP_H264
                    fwrite(payload, payloadsize, 1, file);
#endif
                    h264_decode_struct h264_data;
                    h264_data.data_len = payloadsize;
                    h264_data.data = payload;
                    h264_data.frame_type = 1;
                    h264_data.pts = ntp_timestamp;
                    raop_rtp_mirror->callbacks.video_process(raop_rtp_mirror->callbacks.cls, raop_rtp_mirror->ntp, &h264_data);
                    free(payload_in);
                    free(payload);
                } else if ((payloadtype & 255) == 1) {
                    float mWidthSource = byteutils_get_float(packet, 40);
                    float mHeightSource = byteutils_get_float(packet, 44);
                    float mWidth = byteutils_get_float(packet, 56);
                    float mHeight = byteutils_get_float(packet, 60);
                    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "mWidthSource = %f mHeightSource = %f mWidth = %f mHeight = %f", mWidthSource, mHeightSource, mWidth, mHeight);
                    /*int mRotateMode = 0;

                    int p = payloadtype >> 8;
                    if (p == 4) {
                        mRotateMode = 1;
                    } else if (p == 7) {
                        mRotateMode = 3;
                    } else if (p != 0) {
                        mRotateMode = 2;
                    }*/

                    // sps_pps This piece of data is not encrypted
                    unsigned char payload[payloadsize];
                    readstart = 0;
                    do {
                        // Payload data
                        ret = recv(stream_fd, payload + readstart, payloadsize - readstart, 0);
                        readstart = readstart + ret;
                    } while (readstart < payloadsize);
                    h264codec_t h264;
                    h264.version = payload[0];
                    h264.profile_high = payload[1];
                    h264.compatibility = payload[2];
                    h264.level = payload[3];
                    h264.reserved_6_and_nal = payload[4];
                    h264.reserved_3_and_sps = payload[5];
                    h264.length_of_sps = (short) (((payload[6] & 255) << 8) + (payload[7] & 255));
                    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "lengthofSPS = %d", h264.length_of_sps);
                    h264.sequence = malloc(h264.length_of_sps);
                    memcpy(h264.sequence, payload + 8, h264.length_of_sps);
                    h264.number_of_pps = payload[h264.length_of_sps + 8];
                    h264.length_of_pps = (short) (((payload[h264.length_of_sps + 9] & 2040) + payload[h264.length_of_sps + 10]) & 255);
                    h264.picture_parameter_set = malloc(h264.length_of_pps);
                    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "lengthofPPS = %d", h264.length_of_pps);
                    memcpy(h264.picture_parameter_set, payload + h264.length_of_sps + 11, h264.length_of_pps);
                    if (h264.length_of_sps + h264.length_of_pps < 102400) {
                        // Copy spspps
                        int sps_pps_len = (h264.length_of_sps + h264.length_of_pps) + 8;
                        unsigned char sps_pps[sps_pps_len];
                        sps_pps[0] = 0;
                        sps_pps[1] = 0;
                        sps_pps[2] = 0;
                        sps_pps[3] = 1;
                        memcpy(sps_pps + 4, h264.sequence, h264.length_of_sps);
                        sps_pps[h264.length_of_sps + 4] = 0;
                        sps_pps[h264.length_of_sps + 5] = 0;
                        sps_pps[h264.length_of_sps + 6] = 0;
                        sps_pps[h264.length_of_sps + 7] = 1;
                        memcpy(sps_pps + h264.length_of_sps + 8, h264.picture_parameter_set, h264.length_of_pps);
#ifdef DUMP_H264
                        fwrite(sps_pps, sps_pps_len, 1, file);
#endif
                        h264_decode_struct h264_data;
                        h264_data.data_len = sps_pps_len;
                        h264_data.data = sps_pps;
                        h264_data.frame_type = 0;
                        h264_data.pts = 0;
                        raop_rtp_mirror->callbacks.video_process(raop_rtp_mirror->callbacks.cls, raop_rtp_mirror->ntp, &h264_data);
                    }
                    free(h264.picture_parameter_set);
                    free(h264.sequence);
                } else if (payloadtype == (short) 2) {
                    readstart = 0;
                    if (payloadsize > 0) {
                        unsigned char* payload_in = malloc(payloadsize);
                        do {
                            ret = recv(stream_fd, payload_in + readstart, payloadsize - readstart, 0);
                            readstart = readstart + ret;
                        } while (readstart < payloadsize);
                    }
                } else if (payloadtype == (short) 4) {
                    readstart = 0;
                    if (payloadsize > 0) {
                        unsigned char* payload_in = malloc(payloadsize);
                        do {
                            ret = recv(stream_fd, payload_in + readstart, payloadsize - readstart, 0);
                            readstart = readstart + ret;
                        } while (readstart < payloadsize);
                    }
                } else {
                    readstart = 0;
                    if (payloadsize > 0) {
                        unsigned char* payload_in = malloc(payloadsize);
                        do {
                            ret = recv(stream_fd, payload_in + readstart, payloadsize - readstart, 0);
                            readstart = readstart + ret;
                        } while (readstart < payloadsize);
                    }
                }
            }
            memset(packet, 0 , 128);
            readstart = 0;
        }
    }

    /* Close the stream file descriptor */
    if (stream_fd != -1) {
        closesocket(stream_fd);
    }
    logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Exiting TCP raop_rtp_mirror_thread thread");
#ifdef DUMP_H264
    fclose(file);
    fclose(file_source);
    fclose(file_len);
#endif
    return 0;
}

void
raop_rtp_start_mirror(raop_rtp_mirror_t *raop_rtp_mirror, int use_udp, unsigned short *mirror_data_lport)
{
    int use_ipv6 = 0;

    assert(raop_rtp_mirror);

    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    if (raop_rtp_mirror->running || !raop_rtp_mirror->joined) {
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        return;
    }

    if (raop_rtp_mirror->remote_saddr.ss_family == AF_INET6) {
        use_ipv6 = 1;
    }
    use_ipv6 = 0;
    if (raop_rtp_init_mirror_sockets(raop_rtp_mirror, use_ipv6) < 0) {
        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Initializing sockets failed");
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        return;
    }
    if (mirror_data_lport) *mirror_data_lport = raop_rtp_mirror->mirror_data_lport;

    /* Create the thread and initialize running values */
    raop_rtp_mirror->running = 1;
    raop_rtp_mirror->joined = 0;

    THREAD_CREATE(raop_rtp_mirror->thread_mirror, raop_rtp_mirror_thread, raop_rtp_mirror);
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
}

void raop_rtp_mirror_stop(raop_rtp_mirror_t *raop_rtp_mirror) {
    assert(raop_rtp_mirror);

    /* Check that we are running and thread is not
     * joined (should never be while still running) */
    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    if (!raop_rtp_mirror->running || raop_rtp_mirror->joined) {
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        return;
    }
    raop_rtp_mirror->running = 0;
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);

    /* Join the thread */
    THREAD_JOIN(raop_rtp_mirror->thread_mirror);


    if (raop_rtp_mirror->mirror_data_sock != -1) closesocket(raop_rtp_mirror->mirror_data_sock);

    /* Mark thread as joined */
    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    raop_rtp_mirror->joined = 1;
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
}

void raop_rtp_mirror_destroy(raop_rtp_mirror_t *raop_rtp_mirror) {
    if (raop_rtp_mirror) {
        raop_rtp_mirror_stop(raop_rtp_mirror);
        MUTEX_DESTROY(raop_rtp_mirror->run_mutex);
        mirror_buffer_destroy(raop_rtp_mirror->buffer);
    }
}

static int
raop_rtp_init_mirror_sockets(raop_rtp_mirror_t *raop_rtp_mirror, int use_ipv6)
{
    int dsock = -1;
    unsigned short dport = 0;

    assert(raop_rtp_mirror);

    dsock = netutils_init_socket(&dport, use_ipv6, 0);
    if (dsock == -1) {
        goto sockets_cleanup;
    }

    /* Listen to the data socket if using TCP */
    if (listen(dsock, 1) < 0)
        goto sockets_cleanup;


    /* Set socket descriptors */
    raop_rtp_mirror->mirror_data_sock = dsock;

    /* Set port values */
    raop_rtp_mirror->mirror_data_lport = dport;
    return 0;

    sockets_cleanup:
    if (dsock != -1) closesocket(dsock);
    return -1;
}
