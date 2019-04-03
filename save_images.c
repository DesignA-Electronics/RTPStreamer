#include <SDL.h>
#include <SDL_image.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAXLINE 10000

static int save_images = 0;
static int save_headers = 0;
static int dump_stats = 0;

#if 0
/*
 * Table K.1 from JPEG spec.
 */
static const int jpeg_luma_quantizer[64] = {
        16, 11, 10, 16, 24, 40, 51, 61,
        12, 12, 14, 19, 26, 58, 60, 55,
        14, 13, 16, 24, 40, 57, 69, 56,
        14, 17, 22, 29, 51, 87, 80, 62,
        18, 22, 37, 56, 68, 109, 103, 77,
        24, 35, 55, 64, 81, 104, 113, 92,
        49, 64, 78, 87, 103, 121, 120, 101,
        72, 92, 95, 98, 112, 100, 103, 99
};

/*
 * Table K.2 from JPEG spec.
 */
static const int jpeg_chroma_quantizer[64] = {
        17, 18, 24, 47, 99, 99, 99, 99,
        18, 21, 26, 66, 99, 99, 99, 99,
        24, 26, 56, 99, 99, 99, 99, 99,
        47, 66, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99
};
#else // From EMOS documentation
static const int jpeg_luma_quantizer[64] = {
    16, 11, 12,  14,  12,  10, 16, 14,  13,  14,  18,  17,  16, 19,  24,  40,
    26, 24, 22,  22,  24,  49, 35, 37,  29,  40,  58,  51,  61, 60,  57,  51,
    56, 55, 64,  72,  92,  78, 64, 68,  87,  69,  55,  56,  80, 109, 81,  87,
    95, 98, 103, 104, 103, 62, 77, 113, 121, 112, 100, 120, 92, 101, 103, 99,
};

static const int jpeg_chroma_quantizer[64] = {
    17, 18, 18, 24, 21, 24, 47, 26, 26, 47, 99, 66, 56, 66, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};
#endif

/*
 * Call MakeTables with the Q factor and two u_char[64] return arrays
 */
static void MakeTables(int q, u_char *lqt, u_char *cqt)
{
    int i;
    int factor = q;

    if (q < 1)
        factor = 1;
    if (q > 99)
        factor = 99;
    if (q < 50)
        q = 5000 / factor;
    else
        q = 200 - factor * 2;

    for (i = 0; i < 64; i++) {
        int lq = (jpeg_luma_quantizer[i] * q + 50) / 100;
        int cq = (jpeg_chroma_quantizer[i] * q + 50) / 100;

        /* Limit the quantizers to 1 <= q <= 255 */
        if (lq < 1)
            lq = 1;
        else if (lq > 255)
            lq = 255;
        lqt[i] = lq;

        if (cq < 1)
            cq = 1;
        else if (cq > 255)
            cq = 255;
        cqt[i] = cq;
    }
}

static u_char lum_dc_codelens[] = {
    0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,
};

static u_char lum_dc_symbols[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
};

static u_char lum_ac_codelens[] = {
    0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d,
};

static u_char lum_ac_symbols[] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

static u_char chm_dc_codelens[] = {
    0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
};

static u_char chm_dc_symbols[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
};

static u_char chm_ac_codelens[] = {
    0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77,
};

static u_char chm_ac_symbols[] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
    0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

static u_char *MakeQuantHeader(u_char *p, u_char *qt, int tableNo,
                               int table_len)
{
    *p++ = 0xff;
    *p++ = 0xdb;          /* DQT */
    *p++ = 0;             /* length msb */
    *p++ = table_len + 3; /* length lsb */
    *p++ = tableNo;
    memcpy(p, qt, table_len);
    return (p + table_len);
}

static u_char *MakeHuffmanHeader(u_char *p, u_char *codelens, int ncodes,
                                 u_char *symbols, int nsymbols, int tableNo,
                                 int tableClass)
{
    *p++ = 0xff;
    *p++ = 0xc4;                  /* DHT */
    *p++ = 0;                     /* length msb */
    *p++ = 3 + ncodes + nsymbols; /* length lsb */
    *p++ = (tableClass << 4) | tableNo;
    memcpy(p, codelens, ncodes);
    p += ncodes;
    memcpy(p, symbols, nsymbols);
    p += nsymbols;
    return (p);
}

static u_char *MakeDRIHeader(u_char *p, u_short dri)
{
    *p++ = 0xff;
    *p++ = 0xdd;       /* DRI */
    *p++ = 0x0;        /* length msb */
    *p++ = 4;          /* length lsb */
    *p++ = dri >> 8;   /* dri msb */
    *p++ = dri & 0xff; /* dri lsb */
    return (p);
}

/*
 *  Arguments:
 *    type, width, height: as supplied in RTP/JPEG header
 *    lqt, cqt: quantization tables as either derived from
 *         the Q field using MakeTables() or as specified
 *         in section 4.2.
 *    dri: restart interval in MCUs, or 0 if no restarts.
 *
 *    p: pointer to return area
 *
 *  Return value:
 *    The length of the generated headers.
 *
 *    Generate a frame and scan headers that can be prepended to the
 *    RTP/JPEG data payload to produce a JPEG compressed image in
 *    interchange format (except for possible trailing garbage and
 *    absence of an EOI marker to terminate the scan).
 */
static int MakeHeaders(u_char *p, int type, int w, int h, u_char *lqt,
                       u_char *cqt, unsigned table_len, u_short dri)
{
    u_char *start = p;

    /* convert from blocks to pixels */
    w <<= 3;
    h <<= 3;

    *p++ = 0xff;
    *p++ = 0xd8; /* SOI */

    if (table_len > 64) {
        p = MakeQuantHeader(p, lqt, 0, table_len / 2);
        p = MakeQuantHeader(p, cqt, 1, table_len / 2);
    } else {
        p = MakeQuantHeader(p, lqt, 0, table_len);
        // p = MakeQuantHeader(p, lqt, 1, table_len);
    }
    if (dri != 0)
        p = MakeDRIHeader(p, dri);

    *p++ = 0xff;
    *p++ = 0xc0;   /* SOF */
    *p++ = 0;      /* length msb */
    *p++ = 17;     /* length lsb */
    *p++ = 8;      /* 8-bit precision */
    *p++ = h >> 8; /* height msb */
    *p++ = h;      /* height lsb */
    *p++ = w >> 8; /* width msb */
    *p++ = w;      /* wudth lsb */
    *p++ = 3;      /* number of components */
    *p++ = 0;      /* comp 0 */
    if (type == 0)
        *p++ = 0x21; /* hsamp = 2, vsamp = 1 */
    else
        *p++ = 0x22;                      /* hsamp = 2, vsamp = 2 */
    *p++ = 0;                             /* quant table 0 */
    *p++ = 1;                             /* comp 1 */
    *p++ = 0x11;                          /* hsamp = 1, vsamp = 1 */
    *p++ = table_len <= 64 ? 0x00 : 0x01; // 1   /* quant table 1 */
    *p++ = 2;                             /* comp 2 */
    *p++ = 0x11;                          /* hsamp = 1, vsamp = 1 */
    *p++ = table_len <= 64 ? 0x00 : 0x01; // 1;  /* quant table 1 */
    p = MakeHuffmanHeader(p, lum_dc_codelens, sizeof(lum_dc_codelens),
                          lum_dc_symbols, sizeof(lum_dc_symbols), 0, 0);
    p = MakeHuffmanHeader(p, lum_ac_codelens, sizeof(lum_ac_codelens),
                          lum_ac_symbols, sizeof(lum_ac_symbols), 0, 1);
    p = MakeHuffmanHeader(p, chm_dc_codelens, sizeof(chm_dc_codelens),
                          chm_dc_symbols, sizeof(chm_dc_symbols), 1, 0);
    p = MakeHuffmanHeader(p, chm_ac_codelens, sizeof(chm_ac_codelens),
                          chm_ac_symbols, sizeof(chm_ac_symbols), 1, 1);

    *p++ = 0xff;
    *p++ = 0xda; /* SOS */
    *p++ = 0;    /* length msb */
    *p++ = 12;   /* length lsb */
    *p++ = 3;    /* 3 components */
    *p++ = 0;    /* comp 0 */
    *p++ = 0;    /* huffman table 0 */
    *p++ = 1;    /* comp 1 */
    *p++ = 0x11; /* huffman table 1 */
    *p++ = 2;    /* comp 2 */
    *p++ = 0x11; /* huffman table 1 */
    *p++ = 0;    /* first DCT coeff */
    *p++ = 63;   /* last DCT coeff */
    *p++ = 0;    /* sucessive approx. */

    return (p - start);
};

static int decode_rtp_packet(uint8_t *packet, int len, SDL_Renderer *renderer,
                             SDL_Texture **texture)
{
    static int last_timestamp = 0;
    static uint8_t buffer[100 * 1024];
    ;
    static int pos = 0;

    uint8_t *rfc2435 = &packet[12];
    uint8_t *payload = &rfc2435[8];
    int payload_len = len - 20;
    if (len < 100)
        return -EINVAL;
    int marker = (packet[1] & 0x80) ? 1 : 0;
    int payload_type = packet[1] & 0x7f;
    if (payload_type != 0x1a) {
        printf("not jpeg: %d 0x%x\n", payload_type, payload_type);
        return -EINVAL;
    }
    int sequence = packet[2] << 8 | packet[3];
    int timestamp =
        packet[4] << 24 | packet[5] << 16 | packet[6] << 8 | packet[7];
    int offset = rfc2435[1] << 16 | rfc2435[2] << 8 | rfc2435[3];
    int type = rfc2435[4];
    int quality = rfc2435[5];
    int width = rfc2435[6];
    int height = rfc2435[7];

    if (dump_stats) {
        printf("marker: %d (0x%x) sequence: %d offset: %d\n", marker,
               packet[0], sequence, offset);
        if (marker) {
            printf("timestamp: %d diff: %d\n", timestamp,
                   timestamp - last_timestamp);
            last_timestamp = timestamp;
        }
    }
    // printf("offset: %d len: %d, total: %d vs %d\n", offset, payload_len,
    // offset + payload_len, sizeof(buffer));
    memcpy(&buffer[offset], payload, payload_len);
    pos = offset + payload_len;

    if (marker) {
        if (pos > 0) {
            static int index = 0;
            uint8_t tmp[4096];
            uint8_t lqt_cqt[128];
            MakeTables(quality, lqt_cqt, lqt_cqt + 64);
            int len = MakeHeaders(tmp, type, width, height, lqt_cqt,
                                  lqt_cqt + 64, 128, 0);

            if (save_images) {
                char name[256];
                sprintf(name, "image-%5.5d.jpg", index);
                FILE *fp = fopen(name, "wb");
                fwrite(tmp, len, 1, fp);
                fwrite(buffer, pos, 1, fp);
                fclose(fp);
            }

            if (save_headers) {
                char name[256];
                sprintf(name, "header-%5.5d.bin", index);
                FILE *fp = fopen(name, "wb");
                fwrite(tmp, len, 1, fp);
                fclose(fp);
            }

            uint8_t combined[1 * 1024 * 1024];
            memcpy(combined, tmp, len);
            memcpy(combined + len, buffer, pos);
            SDL_RWops *rw = SDL_RWFromMem(combined, pos + len);
            *texture = IMG_LoadTexture_RW(renderer, rw, 1);

            index++;
        }
    }

    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-i] [-s] [-p port]\n"
            "  -i: save images as jpeg files\n"
            "  -s: save jpeg headers\n"
            "  -p: change port (default 50004)\n"
            "  -d: dump stats\n",
            prog);
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
    int sockfd;
    uint8_t buffer[MAXLINE];
    struct sockaddr_in servaddr;
    int optval;
    time_t start = time(NULL);
    int nframes = 0;
    int port = 50004;
    int opt;

    while ((opt = getopt(argc, argv, "isp:d")) != -1) {
        switch (opt) {
        case 'i':
            save_images = 1;
            break;
        case 's':
            save_headers = 1;
            break;
        case 'p':
            port = atoi(optarg);
            break;
        case 'd':
            dump_stats = 1;
            break;
        default:
            usage(argv[0]);
        }
    }

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval,
               sizeof(int));

    memset(&servaddr, 0, sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("ERROR on binding");
        exit(EXIT_FAILURE);
    }

    SDL_Init(SDL_INIT_VIDEO);
    IMG_Init(IMG_INIT_JPG);

    SDL_Window *window =
        SDL_CreateWindow("RTP Camera streamer", SDL_WINDOWPOS_UNDEFINED,
                         SDL_WINDOWPOS_UNDEFINED, 800, 600, 0);

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, 0);

    int done = 0;
    while (!done) {
        int n;
        unsigned int len;
        len = sizeof(servaddr);

        n = recvfrom(sockfd, (char *)buffer, sizeof(buffer), 0,
                     (struct sockaddr *)&servaddr, &len);

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                done = 1;
        }
        SDL_Texture *texture = NULL;
        decode_rtp_packet(buffer, n, renderer, &texture);

        if (texture) {
            SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_DestroyTexture(texture);
            SDL_RenderPresent(renderer);
            nframes++;
            if (nframes % 50 == 0) {
                time_t elapsed = time(NULL) - start;
                printf("Received %d frames in %lds (%f fps)\n", nframes,
                       elapsed,
                       elapsed ? (float)nframes / (float)elapsed : -1);
            }
        }
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    SDL_Quit();

    close(sockfd);
    return 0;
}
