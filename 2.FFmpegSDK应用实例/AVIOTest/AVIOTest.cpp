
#include <stdio.h>
#include <stdlib.h>

extern "C"
{
#include <libavutil/avutil.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

typedef uint8_t BYTE;

FILE *input_file = nullptr;
BYTE *file_buffer = nullptr, *avio_ctx_buffer = nullptr;
size_t file_buffer_size = 0, avio_ctx_buffer_size = 16;

typedef struct buffer_data
{
	BYTE *ptr;
	size_t size; ///< size left in the buffer
} BufferData;

BufferData block_data = { 0 };

static int read_packet(void *opaque, uint8_t *buf, int buf_size)
{
	struct buffer_data *bd = (struct buffer_data *)opaque;
	buf_size = FFMIN(buf_size, bd->size);

	printf("ptr:%p size:%zu\n", bd->ptr, bd->size);

	/* copy internal buffer data to buf */
	memcpy(buf, bd->ptr, buf_size);
	bd->ptr += buf_size;
	bd->size -= buf_size;

	return buf_size;
}

int main(int argc, char** argv)
{
	AVIOContext *avio_ctx = nullptr;

	if (argc < 2)
	{
		printf("Error: input file name missing...\n");
		return -1;
	}

	input_file = fopen(argv[1], "rb");
	if (!input_file)
	{
		printf("Error: opening input file failed.\n");
		return -1;
	}
	
	fseek(input_file, 0, SEEK_END);
	file_buffer_size = ftell(input_file);
	fseek(input_file, 0, SEEK_SET);
	file_buffer = (BYTE *)av_mallocz(file_buffer_size);
	fread(file_buffer, 1, file_buffer_size, input_file);

	BufferData bd = { file_buffer, file_buffer_size };


	avio_ctx_buffer = (BYTE *)av_malloc(avio_ctx_buffer_size);
	avio_ctx = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size, 0, &bd, read_packet, NULL, NULL);
	if (!avio_ctx)
	{
		printf("Error: allocating AVIOContext failed.\n");
		goto end;
	}

	printf("Output in main 1:\n");
	for (int i = 0; i < 8; i++) {
		printf("0x%x\t", avio_r8(avio_ctx));
		if ((i+1) % 8 == 0)	{
			printf("\n");
		}
	}
	
end:
	if (avio_ctx) {
		av_freep(&avio_ctx->buffer);
		av_freep(&avio_ctx);
	}
	fclose(input_file);
	input_file = nullptr;
	return 0;
}
