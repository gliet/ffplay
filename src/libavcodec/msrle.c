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

static void msrle_decode_pal4(MsrleContext *s)//�и�s->avctx->height���п�s->frame.linesize[0]��ѹ������s->buf; ���������������s->frame.data[0]
{//�Ӵ��뿴������ʱ��ÿ�зֿ�����ģ�ÿ�н��������л��п����� 0 0
    int stream_ptr = 0;
    unsigned char rle_code;
    unsigned char extra_byte, odd_pixel;
    unsigned char stream_byte;                    //��ȡѹ�����ݵ���ʱ����
    int pixel_ptr = 0;                            //��ƫ�ƣ�����ڱ��е�һ�����ص�ƫ��
    int row_dec = s->frame.linesize[0];           //�п�
    int row_ptr = (s->avctx->height - 1) *row_dec;//��ƫ�ƣ�ָ��ÿ�еĵ�һ�����أ����е�һ�����������ͼ���һ�����ص�ƫ�ƣ���ʼΪ���һ�е�һ�����أ�
    int frame_size = row_dec * s->avctx->height;  //֡��С= �п�x ����          //��ͼ��߶�Ϊ����ʱ�����ݴ�ͼ����·����Ϸ����档���ݵ����һ����ͼ��ĵ�һ��
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
        if (rle_code == 0)//ת����
        {
            // fetch the next byte to see how to handle escape code
            FETCH_NEXT_STREAM_BYTE();
            if (stream_byte == 0)//�н�����ˢ��row_ptr pxiel_ptr
            {
                // line is done, goto the next one
                row_ptr -= row_dec;
                pixel_ptr = 0;
            }
            else if (stream_byte == 1)//ͼ�����������
            {
                // decode is done
                return ;
            }
            else if (stream_byte == 2)//������ת�����������ֽڱ�ʾ����ƫ��
            {
                // reposition frame decode coordinates
                FETCH_NEXT_STREAM_BYTE();
                pixel_ptr += stream_byte;
                FETCH_NEXT_STREAM_BYTE();
                row_ptr -= stream_byte * row_dec;
            }
            else  //����ģʽ�����µ�stream_byte�����ص����ݣ��Ƿ�ѹ�����ݵ�ֱ�Ӵ洢��
            {
                // copy pixels from encoded stream
                odd_pixel = stream_byte &1;      //�ж��ǲ�������������
                rle_code = (stream_byte + 1) / 2;//������������ż�������أ���Ҫ(stream_byte + 1) / 2���ֽڴ洢��
                extra_byte = rle_code &0x01;     //ѹ��������Ҫ˫�ֽڶ��룬���rle_code����������Ҫ���һ���ֽ�
                if ((row_ptr + pixel_ptr + stream_byte > frame_size) || (row_ptr < 0))
                {     //���ָ�� ������Χ������
                    return ;
                }

                for (i = 0; i < rle_code; i++)//������rle_code�����ݣ���ÿ���ֽ������������ֵȡ������
                {
                    if (pixel_ptr >= s->avctx->width)//��������п�break��ò��ʼ����false
                        break;
                    FETCH_NEXT_STREAM_BYTE();
                    s->frame.data[0][row_ptr + pixel_ptr] = stream_byte >> 4;//ȡ��4λ��ÿ����ֽڱ�ʾһ������
                    pixel_ptr++;
                    if (i + 1 == rle_code && odd_pixel)//������������أ����һ��stream_byteֻȡ��4λ�����ݣ�����λ��Ч
                        break;
                    if (pixel_ptr >= s->avctx->width)//ò��ʼ����false
                        break;
                    s->frame.data[0][row_ptr + pixel_ptr] = stream_byte &0x0F;//ȡ��4λ��
                    pixel_ptr++;
                }

                // if the RLE code is odd, skip a byte in the stream
                if (extra_byte)//���ѹ�����������������������һ������ֽ�
                    stream_ptr++;
            }
        }
        else //����ģʽ����������rle_code�����ض�����һ���ֽ����������ص��ظ���
        {
            // decode a run of data
            if ((row_ptr + pixel_ptr + stream_byte > frame_size) || (row_ptr < 0))
            {
                return ;
            }
            FETCH_NEXT_STREAM_BYTE();
            for (i = 0; i < rle_code; i++)
            {
                if (pixel_ptr >= s->avctx->width)//ò��ʼ����false
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
