
#include "php.h"
#include "php_av.h"

// code adopted from qt-faststart.c

#define FFMIN(a,b) ((a) > (b) ? (b) : (a))

#define BE_16(x) ((((uint8_t*)(x))[0] <<  8) | ((uint8_t*)(x))[1])

#define BE_32(x) ((((uint8_t*)(x))[0] << 24) |  \
                  (((uint8_t*)(x))[1] << 16) |  \
                  (((uint8_t*)(x))[2] <<  8) |  \
                   ((uint8_t*)(x))[3])

#define BE_64(x) (((uint64_t)(((uint8_t*)(x))[0]) << 56) |  \
                  ((uint64_t)(((uint8_t*)(x))[1]) << 48) |  \
                  ((uint64_t)(((uint8_t*)(x))[2]) << 40) |  \
                  ((uint64_t)(((uint8_t*)(x))[3]) << 32) |  \
                  ((uint64_t)(((uint8_t*)(x))[4]) << 24) |  \
                  ((uint64_t)(((uint8_t*)(x))[5]) << 16) |  \
                  ((uint64_t)(((uint8_t*)(x))[6]) <<  8) |  \
                  ((uint64_t)( (uint8_t*)(x))[7]))

#define BE_FOURCC(ch0, ch1, ch2, ch3)           \
    ( (uint32_t)(unsigned char)(ch3)        |   \
     ((uint32_t)(unsigned char)(ch2) <<  8) |   \
     ((uint32_t)(unsigned char)(ch1) << 16) |   \
     ((uint32_t)(unsigned char)(ch0) << 24) )

#define QT_ATOM BE_FOURCC
/* top level atoms */
#define FREE_ATOM QT_ATOM('f', 'r', 'e', 'e')
#define JUNK_ATOM QT_ATOM('j', 'u', 'n', 'k')
#define MDAT_ATOM QT_ATOM('m', 'd', 'a', 't')
#define MOOV_ATOM QT_ATOM('m', 'o', 'o', 'v')
#define PNOT_ATOM QT_ATOM('p', 'n', 'o', 't')
#define SKIP_ATOM QT_ATOM('s', 'k', 'i', 'p')
#define WIDE_ATOM QT_ATOM('w', 'i', 'd', 'e')
#define PICT_ATOM QT_ATOM('P', 'I', 'C', 'T')
#define FTYP_ATOM QT_ATOM('f', 't', 'y', 'p')
#define UUID_ATOM QT_ATOM('u', 'u', 'i', 'd')

#define CMOV_ATOM QT_ATOM('c', 'm', 'o', 'v')
#define STCO_ATOM QT_ATOM('s', 't', 'c', 'o')
#define CO64_ATOM QT_ATOM('c', 'o', '6', '4')

#define ATOM_PREAMBLE_SIZE    8
#define COPY_BUFFER_SIZE      1024 * 64

// from libavformat/url.h

typedef struct URLContext URLContext;
int ffurl_read_complete(URLContext *h, unsigned char *buf, int size);
int ffurl_write(URLContext *h, const unsigned char *buf, int size);
int64_t ffurl_seek(URLContext *h, int64_t pos, int whence);

int av_shift_file(URLContext *file, int64_t start_offset, int64_t end_offset, int64_t size) {
	int result = FALSE;
	int64_t bytes_to_move = end_offset - start_offset;
	int buffer_size = (bytes_to_move < COPY_BUFFER_SIZE) ? (int) bytes_to_move : COPY_BUFFER_SIZE;
    unsigned char *copy_buffer =  emalloc(buffer_size);
    int64_t offset;
    int bytes_to_copy = buffer_size;

    offset = end_offset;
    do {
    	if(bytes_to_move < buffer_size) {
    	    bytes_to_copy = (int) bytes_to_move;
    		offset = start_offset;
    	} else {
    		offset -= buffer_size;
    	}

    	// seek to original position
    	if(ffurl_seek(file, offset, SEEK_SET) < 0) {
    		goto error_out;
    	}

    	// read in a chunk
    	if(ffurl_read_complete(file, copy_buffer, bytes_to_copy) != bytes_to_copy) {
    		goto error_out;
    	}

    	// seek to new position
    	if(ffurl_seek(file, offset + size, SEEK_SET) < 0) {
    		goto error_out;
    	}

    	// write the chunk
    	if(ffurl_write(file, copy_buffer, bytes_to_copy) != bytes_to_copy) {
    		goto error_out;
    	}

    	bytes_to_move -= bytes_to_copy;
    } while(bytes_to_move > 0);

	result = TRUE;
error_out:
	efree(copy_buffer);
	return result;
}

int av_optimize_mov_file(AVIOContext *pb) {
	int result = FALSE;
    URLContext *file  = pb->opaque;
    unsigned char atom_bytes[ATOM_PREAMBLE_SIZE];
    uint32_t atom_type   = 0;
    uint64_t atom_size   = 0;
    uint64_t atom_offset = 0;
    uint64_t last_offset;
    unsigned char *moov_atom = NULL;
    unsigned char *ftyp_atom = NULL;
    uint64_t moov_atom_size;
    uint64_t ftyp_atom_size = 0;
    uint64_t i, j;
    uint32_t offset_count;
    uint64_t current_offset;
    int64_t start_offset = 0;

    ffurl_seek(file, 0, SEEK_SET);

    /* traverse through the atoms in the file to make sure that 'moov' is
     * at the end */
    for(;;) {
        if (ffurl_read_complete(file, atom_bytes, ATOM_PREAMBLE_SIZE) != ATOM_PREAMBLE_SIZE) {
            break;
        }
        atom_size = (uint32_t) BE_32(&atom_bytes[0]);
        atom_type = BE_32(&atom_bytes[4]);

        /* keep ftyp atom */
        if (atom_type == FTYP_ATOM) {
            ftyp_atom_size = atom_size;
            if(ftyp_atom) {
            	efree(ftyp_atom);
            }
            ftyp_atom = emalloc((size_t) ftyp_atom_size);
            start_offset = ffurl_seek(file, -ATOM_PREAMBLE_SIZE, SEEK_CUR);
            if (start_offset < 0) {
                goto error_out;
            }
        	if (ffurl_read_complete(file, ftyp_atom, (int) atom_size) != atom_size) {
                goto error_out;
            }
        } else {
            int64_t ret;
            /* 64-bit special case */
            if (atom_size == 1) {
                if (ffurl_read_complete(file, atom_bytes, ATOM_PREAMBLE_SIZE) != ATOM_PREAMBLE_SIZE) {
                    break;
                }
                atom_size = BE_64(&atom_bytes[0]);
                ret = ffurl_seek(file, atom_size - ATOM_PREAMBLE_SIZE * 2, SEEK_CUR);
            } else {
                ret = ffurl_seek(file, atom_size - ATOM_PREAMBLE_SIZE, SEEK_CUR);
            }
            if(ret < 0) {
                goto error_out;
            }
        }
        if ((atom_type != FREE_ATOM) &&
            (atom_type != JUNK_ATOM) &&
            (atom_type != MDAT_ATOM) &&
            (atom_type != MOOV_ATOM) &&
            (atom_type != PNOT_ATOM) &&
            (atom_type != SKIP_ATOM) &&
            (atom_type != WIDE_ATOM) &&
            (atom_type != PICT_ATOM) &&
            (atom_type != UUID_ATOM) &&
            (atom_type != FTYP_ATOM)) {
            break;
        }
        atom_offset += atom_size;

        /* The atom header is 8 (or 16 bytes), if the atom size (which
         * includes these 8 or 16 bytes) is less than that, we won't be
         * able to continue scanning sensibly after this atom, so break. */
        if (atom_size < 8)
            break;
    }

    if (atom_type != MOOV_ATOM) {
        goto error_out;
    }

    /* moov atom was, in fact, the last atom in the chunk; load the whole
     * moov atom */
    if (ffurl_seek(file, - ((int64_t) atom_size), SEEK_END) < 0) {
        goto error_out;
    }
    last_offset    = ffurl_seek(file, 0, SEEK_CUR);
    moov_atom_size = atom_size;
    moov_atom      = emalloc((size_t) moov_atom_size);
    if (ffurl_read_complete(file, moov_atom, (int) moov_atom_size) != moov_atom_size) {
        goto error_out;
    }

    /* this utility does not support compressed atoms yet, so disqualify
     * files with compressed QT atoms */
    if (BE_32(&moov_atom[12]) == CMOV_ATOM) {
        goto error_out;
    }

    /* crawl through the moov chunk in search of stco or co64 atoms */
    for (i = 4; i < moov_atom_size - 4; i++) {
        atom_type = BE_32(&moov_atom[i]);
        if (atom_type == STCO_ATOM) {
            atom_size = BE_32(&moov_atom[i - 4]);
            if (i + atom_size - 4 > moov_atom_size) {
                goto error_out;
            }
            offset_count = BE_32(&moov_atom[i + 8]);
            if (i + 12LL + offset_count * 4LL > moov_atom_size) {
                goto error_out;
            }
            for (j = 0; j < offset_count; j++) {
                current_offset  = BE_32(&moov_atom[i + 12 + j * 4]);
                current_offset += moov_atom_size;
                moov_atom[i + 12 + j * 4 + 0] = (current_offset >> 24) & 0xFF;
                moov_atom[i + 12 + j * 4 + 1] = (current_offset >> 16) & 0xFF;
                moov_atom[i + 12 + j * 4 + 2] = (current_offset >>  8) & 0xFF;
                moov_atom[i + 12 + j * 4 + 3] = (current_offset >>  0) & 0xFF;
            }
            i += atom_size - 4;
        } else if (atom_type == CO64_ATOM) {
            atom_size = BE_32(&moov_atom[i - 4]);
            if (i + atom_size - 4 > moov_atom_size) {
                goto error_out;
            }
            offset_count = BE_32(&moov_atom[i + 8]);
            if (i + 12LL + offset_count * 8LL > moov_atom_size) {
                goto error_out;
            }
            for (j = 0; j < offset_count; j++) {
                current_offset  = BE_64(&moov_atom[i + 12 + j * 8]);
                current_offset += moov_atom_size;
                moov_atom[i + 12 + j * 8 + 0] = (current_offset >> 56) & 0xFF;
                moov_atom[i + 12 + j * 8 + 1] = (current_offset >> 48) & 0xFF;
                moov_atom[i + 12 + j * 8 + 2] = (current_offset >> 40) & 0xFF;
                moov_atom[i + 12 + j * 8 + 3] = (current_offset >> 32) & 0xFF;
                moov_atom[i + 12 + j * 8 + 4] = (current_offset >> 24) & 0xFF;
                moov_atom[i + 12 + j * 8 + 5] = (current_offset >> 16) & 0xFF;
                moov_atom[i + 12 + j * 8 + 6] = (current_offset >>  8) & 0xFF;
                moov_atom[i + 12 + j * 8 + 7] = (current_offset >>  0) & 0xFF;
            }
            i += atom_size - 4;
        }
    }

    // shift everything from (start_offset + ftyp_atom_size) to (last_offset)
    // forward by moov_atom_size
    av_shift_file(file, start_offset + ftyp_atom_size, last_offset, moov_atom_size);

    // write the updated moov atom
    ffurl_seek(file, start_offset + ftyp_atom_size, SEEK_SET);
    ffurl_write(file, moov_atom, (int) moov_atom_size);

    result = TRUE;

error_out:
	if(moov_atom) {
		efree(moov_atom);
	}
	if(ftyp_atom) {
		efree(ftyp_atom);
	}
    return result;
}

// these functions are not public in libav for some reason
#if !defined(HAVE_FFURL_READ_COMPLETE) || !defined(HAVE_FFURL_WRITE) || !defined(HAVE_FFURL_SEEK)
#if LIBAVFORMAT_VERSION_MAJOR > 53
struct URLContext {
    const AVClass *av_class;    /**< information for av_log(). Set by url_open(). */
    struct URLProtocol *prot;
    void *priv_data;
    char *filename;             /**< specified URL */
    int flags;
    int max_packet_size;        /**< if non zero, the stream is packetized with this max packet size */
    int is_streamed;            /**< true if streamed (no seek possible), default = false */
    int is_connected;
    AVIOInterruptCB interrupt_callback;
};

typedef struct URLProtocol {
    const char *name;
    int     (*url_open)( URLContext *h, const char *url, int flags);
    /**
     * This callback is to be used by protocols which open further nested
     * protocols. options are then to be passed to ffurl_open()/ffurl_connect()
     * for those nested protocols.
     */
    int     (*url_open2)(URLContext *h, const char *url, int flags, AVDictionary **options);

    /**
     * Read data from the protocol.
     * If data is immediately available (even less than size), EOF is
     * reached or an error occurs (including EINTR), return immediately.
     * Otherwise:
     * In non-blocking mode, return AVERROR(EAGAIN) immediately.
     * In blocking mode, wait for data/EOF/error with a short timeout (0.1s),
     * and return AVERROR(EAGAIN) on timeout.
     * Checking interrupt_callback, looping on EINTR and EAGAIN and until
     * enough data has been read is left to the calling function; see
     * retry_transfer_wrapper in avio.c.
     */
    int     (*url_read)( URLContext *h, unsigned char *buf, int size);
    int     (*url_write)(URLContext *h, unsigned char *buf, int size);
    int64_t (*url_seek)( URLContext *h, int64_t pos, int whence);
    int     (*url_close)(URLContext *h);
    struct URLProtocol *next;
    int (*url_read_pause)(URLContext *h, int pause);
    int64_t (*url_read_seek)(URLContext *h, int stream_index,
                             int64_t timestamp, int flags);
    int (*url_get_file_handle)(URLContext *h);
    int (*url_get_multi_file_handle)(URLContext *h, int **handles,
                                     int *numhandles);
    int (*url_shutdown)(URLContext *h, int flags);
    int priv_data_size;
    const AVClass *priv_data_class;
    int flags;
    int (*url_check)(URLContext *h, int mask);
} URLProtocol;
#endif
#endif

#if !defined(HAVE_FFURL_READ_COMPLETE) || !defined(HAVE_FFURL_WRITE)
int ff_check_interrupt(AVIOInterruptCB *cb)
{
    int ret;
    if (cb && cb->callback && (ret = cb->callback(cb->opaque)))
        return ret;
    return 0;
}

static inline int retry_transfer_wrapper(URLContext *h, unsigned char *buf, int size, int size_min,
                                         int (*transfer_func)(URLContext *h, unsigned char *buf, int size))
{
    int ret, len;
    int fast_retries = 5;

    len = 0;
    while (len < size_min) {
        ret = transfer_func(h, buf+len, size-len);
        if (ret == AVERROR(EINTR))
            continue;
        if (h->flags & AVIO_FLAG_NONBLOCK)
            return ret;
        if (ret == AVERROR(EAGAIN)) {
            ret = 0;
            if (fast_retries)
                fast_retries--;
            else
                usleep(1000);
        } else if (ret < 1)
            return ret < 0 ? ret : len;
        if (ret)
           fast_retries = FFMAX(fast_retries, 2);
        len += ret;
        if (ff_check_interrupt(&h->interrupt_callback))
            return AVERROR_EXIT;
    }
    return len;
}
#endif

#if !defined(HAVE_FFURL_READ_COMPLETE)
int ffurl_read_complete(URLContext *h, unsigned char *buf, int size)
{
    if (!(h->flags & AVIO_FLAG_READ))
        return AVERROR(EIO);
    return retry_transfer_wrapper(h, buf, size, size, h->prot->url_read);
}
#endif

#if !defined(HAVE_FFURL_WRITE)
int ffurl_write(URLContext *h, const unsigned char *buf, int size)
{
    if (!(h->flags & AVIO_FLAG_WRITE))
        return AVERROR(EIO);
    /* avoid sending too big packets */
    if (h->max_packet_size && size > h->max_packet_size)
        return AVERROR(EIO);

    return retry_transfer_wrapper(h, (unsigned char *) buf, size, size, h->prot->url_write);
}
#endif

#if !defined(HAVE_FFURL_SEEK)
int64_t ffurl_seek(URLContext *h, int64_t pos, int whence)
{
    int64_t ret;

    if (!h->prot->url_seek)
        return AVERROR(ENOSYS);
    ret = h->prot->url_seek(h, pos, whence & ~AVSEEK_FORCE);
    return ret;
}
#endif
