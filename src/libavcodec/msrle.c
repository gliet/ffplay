#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../libavutil/common.h"
#include "avcodec.h"
#include "dsputil.h"

#define FF_BUFFER_HINTS_VALID    0x01 // Buffer hints value is meaningful (if 0 ignore)
#define FF_BUFFER_HINTS_READABLE 0x02 // Codec will read from buffer
#define FF_BUFFER_HINTS_PRESERVE 0x04 // User must not alter buffer content
#define FF_BUFFER_HINTS_REUSABLE 0x08 // Codec will reuse the buffer (update)

typedef struct MsrleContext
{
    AVCodecContext *avctx;
    AVFrame frame;

    unsigned char *buf;
    int size;

} MsrleContext;

#define FETCH_NEXT_STREAM_BYTE() \
	if (stream_ptr >= s->size) \
	{ \
		return; \
	} \
	stream_byte = s->buf[stream_ptr++];

static void msrle_decode_pal4(MsrleContext *s)//行高s->avctx->height、行宽s->frame.linesize[0]、压缩数据s->buf; 解码后的数据输出到s->frame.data[0]
{//从代码看，编码时是每行分开编码的，每行结束都会有换行控制字 0 0
    int stream_ptr = 0;
    unsigned char rle_code;
    unsigned char extra_byte, odd_pixel;
    unsigned char stream_byte;                    //读取压缩数据的临时变量
    int pixel_ptr = 0;                            //列偏移，相对于本行第一个像素的偏移
    int row_dec = s->frame.linesize[0];           //行宽
    int row_ptr = (s->avctx->height - 1) *row_dec;//行偏移，指向每行的第一个像素，各行第一个像素相对于图像第一个像素的偏移，初始为最后一行第一个像素，
    int frame_size = row_dec * s->avctx->height;  //帧大小= 行宽x 行数          //当图像高度为正数时，数据从图像的下方向上方保存。数据的最后一行是图像的第一行
    int i;

    // make the palette available
    memcpy(s->frame.data[1], s->avctx->palctrl->palette, AVPALETTE_SIZE);
    if (s->avctx->palctrl->palette_changed)
    {
//      s->frame.palette_has_changed = 1;
        s->avctx->palctrl->palette_changed = 0;
    }

    while (row_ptr >= 0)
    {
        FETCH_NEXT_STREAM_BYTE();
        rle_code = stream_byte;
        if (rle_code == 0)//转义码
        {
            // fetch the next byte to see how to handle escape code
            FETCH_NEXT_STREAM_BYTE();
            if (stream_byte == 0)//行结束，刷新row_ptr pxiel_ptr
            {
                // line is done, goto the next one
                row_ptr -= row_dec;
                pixel_ptr = 0;
            }
            else if (stream_byte == 1)//图像结束，返回
            {
                // decode is done
                return ;
            }
            else if (stream_byte == 2)//坐标跳转，接下两个字节表示行列偏移
            {
                // reposition frame decode coordinates
                FETCH_NEXT_STREAM_BYTE();
                pixel_ptr += stream_byte;
                FETCH_NEXT_STREAM_BYTE();
                row_ptr -= stream_byte * row_dec;
            }
            else  //绝对模式，接下的stream_byte个像素的数据，是非压缩数据的直接存储。
            {
                // copy pixels from encoded stream
                odd_pixel = stream_byte &1;      //判断是不是奇数个像素
                rle_code = (stream_byte + 1) / 2;//不论奇数还是偶数个像素，需要(stream_byte + 1) / 2个字节存储，
                extra_byte = rle_code &0x01;     //压缩数据需要双字节对齐，如果rle_code是奇数，需要填充一个字节
                if ((row_ptr + pixel_ptr + stream_byte > frame_size) || (row_ptr < 0))
                {     //如果指针 超出范围，返回
                    return ;
                }

                for (i = 0; i < rle_code; i++)//解码这rle_code个数据，把每个字节里的两个像素值取出来，
                {
                    if (pixel_ptr >= s->avctx->width)//如果超出行宽，break。貌似始终是false
                        break;
                    FETCH_NEXT_STREAM_BYTE();
                    s->frame.data[0][row_ptr + pixel_ptr] = stream_byte >> 4;//取高4位。每半个字节表示一个像素
                    pixel_ptr++;
                    if (i + 1 == rle_code && odd_pixel)//如果奇数个像素，最后一个stream_byte只取高4位的数据，低四位无效
                        break;
                    if (pixel_ptr >= s->avctx->width)//貌似始终是false
                        break;
                    s->frame.data[0][row_ptr + pixel_ptr] = stream_byte &0x0F;//取低4位。
                    pixel_ptr++;
                }

                // if the RLE code is odd, skip a byte in the stream
                if (extra_byte)//如果压缩数据是奇数，跳过后面的一个填充字节
                    stream_ptr++;
            }
        }
        else //编码模式，接下来的rle_code个像素都是下一个字节里两个像素的重复。
        {
            // decode a run of data
            if ((row_ptr + pixel_ptr + stream_byte > frame_size) || (row_ptr < 0))
            {
                return ;
            }
            FETCH_NEXT_STREAM_BYTE();
            for (i = 0; i < rle_code; i++)
            {
                if (pixel_ptr >= s->avctx->width)//貌似始终是false
                    break;
                if ((i &1) == 0)
                    s->frame.data[0][row_ptr + pixel_ptr] = stream_byte >> 4;
                else
                    s->frame.data[0][row_ptr + pixel_ptr] = stream_byte &0x0F;
                pixel_ptr++;
            }
        }
    }

    // one last sanity check on the way out
    if (stream_ptr < s->size)
    {
        // error
    }
}

static void msrle_decode_pal8(MsrleContext *s)
{
    int stream_ptr = 0;
    unsigned char rle_code;
    unsigned char extra_byte;
    unsigned char stream_byte;
    int pixel_ptr = 0;
    int row_dec = s->frame.linesize[0];
    int row_ptr = (s->avctx->height - 1) *row_dec;
    int frame_size = row_dec * s->avctx->height;

    // make the palette available
    memcpy(s->frame.data[1], s->avctx->palctrl->palette, AVPALETTE_SIZE);
    if (s->avctx->palctrl->palette_changed)
    {
//      s->frame.palette_has_changed = 1;
        s->avctx->palctrl->palette_changed = 0;
    }

    while (row_ptr >= 0)
    {
        FETCH_NEXT_STREAM_BYTE();
        rle_code = stream_byte;
        if (rle_code == 0)
        {
            // fetch the next byte to see how to handle escape code
            FETCH_NEXT_STREAM_BYTE();
            if (stream_byte == 0)
            {
                // line is done, goto the next one
                row_ptr -= row_dec;
                pixel_ptr = 0;
            }
            else if (stream_byte == 1)
            {
                // decode is done
                return ;
            }
            else if (stream_byte == 2)
            {
                // reposition frame decode coordinates
                FETCH_NEXT_STREAM_BYTE();
                pixel_ptr += stream_byte;
                FETCH_NEXT_STREAM_BYTE();
                row_ptr -= stream_byte * row_dec;
            }
            else
            {
                // copy pixels from encoded stream
                if ((row_ptr + pixel_ptr + stream_byte > frame_size) || (row_ptr < 0))
                {
                    return ;
                }

                rle_code = stream_byte;
                extra_byte = stream_byte &0x01;
                if (stream_ptr + rle_code + extra_byte > s->size)
                {
                    return ;
                }

                while (rle_code--)
                {
                    FETCH_NEXT_STREAM_BYTE();
                    s->frame.data[0][row_ptr + pixel_ptr] = stream_byte;
                    pixel_ptr++;
                }

                // if the RLE code is odd, skip a byte in the stream
                if (extra_byte)
                    stream_ptr++;
            }
        }
        else
        {
            // decode a run of data
            if ((row_ptr + pixel_ptr + stream_byte > frame_size) || (row_ptr < 0))
            {
                return ;
            }

            FETCH_NEXT_STREAM_BYTE();

            while (rle_code--)
            {
                s->frame.data[0][row_ptr + pixel_ptr] = stream_byte;
                pixel_ptr++;
            }
        }
    }

    // one last sanity check on the way out
    if (stream_ptr < s->size)
    {
        // error
    }
}

static int msrle_decode_init(AVCodecContext *avctx)
{
    MsrleContext *s = (MsrleContext*)avctx->priv_data;

    s->avctx = avctx;

    avctx->pix_fmt = PIX_FMT_PAL8;

    s->frame.data[0] = NULL;

    return 0;
}

static int msrle_decode_frame(AVCodecContext *avctx, void *data, int *data_size, uint8_t *buf, int buf_size)
{
    MsrleContext *s = (MsrleContext*)avctx->priv_data;

    s->buf = buf;
    s->size = buf_size;

    if (avctx->reget_buffer(avctx, &s->frame))
        return  - 1;

    switch (avctx->bits_per_sample)
    {
    case 8:
        msrle_decode_pal8(s);
        break;
    case 4:
        msrle_decode_pal4(s);
        break;
    default:
        break;
    }

    *data_size = sizeof(AVFrame);
    *(AVFrame*)data = s->frame;

    // report that the buffer was completely consumed
    return buf_size;
}

static int msrle_decode_end(AVCodecContext *avctx)
{
    MsrleContext *s = (MsrleContext*)avctx->priv_data;

    // release the last frame
    if (s->frame.data[0])
        avctx->release_buffer(avctx, &s->frame);

    return 0;
}

AVCodec msrle_decoder =
{
    "msrle", 
	CODEC_TYPE_VIDEO, 
	CODEC_ID_MSRLE, 
	sizeof(MsrleContext), 
	msrle_decode_init, 
	NULL, 
	msrle_decode_end, 
	msrle_decode_frame
};
/*
typedef struct AVCodec
{
    const char *name;
    enum CodecType type;
    enum CodecID id;
    int priv_data_size;
    int(*init)(AVCodecContext*);
    int(*encode)(AVCodecContext *, uint8_t *buf, int buf_size, void *data);
    int(*close)(AVCodecContext*);
    int(*decode)(AVCodecContext *, void *outdata, int *outdata_size, uint8_t *buf, int buf_size);
    int capabilities;

    struct AVCodec *next;
}AVCodec;

*/
