# FFmpeg_Tutorial
**FFmpeg工具和sdk库的使用demo**

***
##一、使用FFmpeg命令行工具和批处理脚本进行简单的音视频文件编辑

###1、基本介绍

对于每一个从事音视频技术开发的工程师，想必没有一个人对FFmpeg这个名称感到陌生。FFmpeg是一套非常知名的音视频处理的开源工具，它包含了开发完成的工具软件、封装好的函数库以及源代码供我们按需使用。FFmpeg提供了非常强大的功能，可以完成音视频的编码、解码、转码、视频采集、后处理（抓图、水印、封装/解封装、格式转换等），还有流媒体服务等诸多功能，可以说涵盖了音视频开发中绝大多数的领域。原生的FFmpeg是在Linux环境下开发的，但是通过各种方法（比如交叉编译等）可以使它运行在多种平台环境上，具有比较好的可移植性。

FFmpeg项目的官方网址为：[https://ffmpeg.org/](https://ffmpeg.org/)。在它的官网上我们可以找到许多非常有用的内容，如项目的简介、版本更新日志、库和源代码的地址、使用文档等信息。官方的使用文档是我们在开发时必不可少的信息来源，其重要性不言而喻。除了官方网站以外，我们下载的FFmpeg的程序包中也有使用参考文档的离线版本。


###2、FFmpeg组成

构成FFmpeg主要有三个部分，第一部分是四个作用不同的工具软件，分别是：ffmpeg.exe，ffplay.exe，ffserver.exe和ffprobe.exe。

- ffmpeg.exe：音视频转码、转换器
- ffplay.exe：简单的音视频播放器
- ffserver.exe：流媒体服务器
- ffprobe.exe：简单的多媒体码流分析器

第二部分是可以供开发者使用的SDK，为各个不同平台编译完成的库。如果说上面的四个工具软件都是完整成品形式的玩具，那么这些库就相当于乐高积木一样，我们可以根据自己的需求使用这些库开发自己的应用程序。这些库有：


- libavcodec：包含音视频编码器和解码器
- libavutil：包含多媒体应用常用的简化编程的工具，如随机数生成器、数据结构、数学函数等功能
- libavformat：包含多种多媒体容器格式的封装、解封装工具
- libavfilter：包含多媒体处理常用的滤镜功能
- libavdevice：用于音视频数据采集和渲染等功能的设备相关
- libswscale：用于图像缩放和色彩空间和像素格式转换功能
- libswresample：用于音频重采样和格式转换等功能

第三部分是整个工程的源代码，无论是编译出来的可执行程序还是SDK，都是由这些源代码编译出来的。FFmpeg的源代码由C语言实现，主要在Linux平台上进行开发。FFmpeg不是一个孤立的工程，它还存在多个依赖的第三方工程来增强它自身的功能。在当前这一系列的博文/视频中，我们暂时不会涉及太多源代码相关的内容，主要以FFmpeg的工具和SDK的调用为主。到下一系列我们将专门研究如何编译源代码并根据源代码来进行二次开发。


###3、FFMpeg工具的下载和使用

####(1)FFmpeg工具的下载：
在官网上我们可以找到"Download"页面，该页上可以下载FFmpeg的工具、库和源代码等。在选择"Windows Packages"下的Windows Builds后，会跳转到Windows版本的下载页面：

在下载页面上，我们可以看到，对于32位和64位版本，分别提供了三种不同的模式：static、shared和dev

- **static**: 该版本提供了静态版本的FFmpeg工具，将依赖的库生成在了最终的可执行文件中；作为工具而言此版本就可以满足我们的需求；
- **share**: 该版本的工具包括可执行文件和dll，程序运行过程必须依赖于提供的dll文件； 
- **dev**: 提供了库的头文件和dll的引导库；

####(2)ffplay.exe的使用

ffplay是一个极为简单的音视频媒体播放器。ffplay.exe使用了ffmpeg库和SDL库开发成的，可以用作FFmpeg API的测试工具。
ffplay的使用方法，最简单的是直接按照默认格式播放某一个音视频文件或流：

	ffplay.exe  -i ../video/IMG_0886.MOV

除此之外，ffplay还支持传入各种参数来控制播放行为。比较常用的参数有：

- -i input_file：输入文件名
- -x width -y height：控制播放窗口的宽高
- -t duration：控制播放的时长
- -window_title title：播放窗口的标题，默认为输入文件名
- -showmode mode：设置显示模式，0:显示视频;1:显示音频波形；2：显示音频频谱
- -autoexit：设置视频播放完成后自动退出

其他参数可以参考官网的文档：[https://www.ffmpeg.org/ffplay.html](https://www.ffmpeg.org/ffplay.html)或下载包里的文档

####(3)ffprobe的使用
ffprobe可以提供简单的音视频文件分析功能。最简单的方法同ffplay类似：
	
	ffprobe.exe  -i ../video/IMG_0886.MOV

分析完成后，ffprobe会显示音视频文件中包含的每个码流的信息，包括编码格式、像素分辨率、码率、帧率等信息：

![ffprobe](http://cl.ly/2h2l1g1U1m1F/QQ截图20160331231357.png)

####(4)ffmpeg的使用

ffmpeg.exe可谓是整个工程的核心所在，它的主要功能是完成音视频各种各样的转换操作。
视频转码：ffmpeg.exe可以将视频文件由原格式转换为其他格式，如从avi转为mp4等：

	ffmpeg -i ../video/IMG_0886.MOV ../video/output_mpeg4_mp3.avi 

这里，ffmpeg默认将视频编码格式选择为mpeg4，音频转码格式为mp3。如果我们希望保留原始编码，需要增加参数-c copy，表明不做任何转码操作：

	ffmpeg -i ../video/IMG_0886.MOV -c copy ../video/output_copy.avi

如果我们希望将视频转换为其他编码格式，则需要在参数中指定目标格式-c:v libx265或-vcodec libx265。ffmpeg支持的所有编码器格式可以通过以下命令查看：

	ffmpeg.exe -encoders

实际操作：

	ffmpeg -i ../video/IMG_0886.MOV -c:v mjpeg  ../video/output_mjpeg.avi

视频解封装：ffmpeg可以将视频中的音频和视频流分别提取出来。需要在命令行中添加参数-an和-vn，分别表示屏蔽音频和视频流：

	@REM 提取视频流
	ffmpeg -i ../video/IMG_0886.MOV -c:v copy -an ../video/IMG_0886_v.MOV
	@REM 提取音频流
	ffmpeg -i ../video/IMG_0886.MOV -c:a copy -vn ../video/IMG_0886_a.aac

视频截取：使用ffmpeg命令并指定参数-ss和-t，分别表示截取开始时刻和截取时长

	@REM 视频截取
	ffmpeg -ss 5 -t 5 -i ../video/IMG_0886.MOV -c copy ../video/IMG_0886_cut.MOV


---
##二、调用FFmpeg SDK对YUV视频序列进行编码

视频由像素格式编码为码流格式是FFMpeg的一项基本功能。通常，视频编码器的输入视频通常为原始的图像像素值，输出格式为符合某种格式规定的二进制码流。

##1、FFMpeg进行视频编码所需要的结构：

- AVCodec：AVCodec结构保存了一个编解码器的实例，实现实际的编码功能。通常我们在程序中定义一个指向AVCodec结构的指针指向该实例。
- AVCodecContext：AVCodecContext表示AVCodec所代表的上下文信息，保存了AVCodec所需要的一些参数。对于实现编码功能，我们可以在这个结构中设置我们指定的编码参数。通常也是定义一个指针指向AVCodecContext。
- AVFrame：AVFrame结构保存编码之前的像素数据，并作为编码器的输入数据。其在程序中也是一个指针的形式。
- AVPacket：AVPacket表示码流包结构，包含编码之后的码流数据。该结构可以不定义指针，以一个对象的形式定义。

在我们的程序中，我们将这些结构整合在了一个结构体中：

	/*************************************************
	Struct:			CodecCtx
	Description:	FFMpeg编解码器上下文
	*************************************************/
	typedef struct
	{
		AVCodec			*codec;		//指向编解码器实例
		AVFrame			*frame;		//保存解码之后/编码之前的像素数据
		AVCodecContext	*c;			//编解码器上下文，保存编解码器的一些参数设置
		AVPacket		pkt;		//码流包结构，包含编码码流数据
	} CodecCtx;

##2、FFMpeg编码的主要步骤：

###(1)、输入编码参数

这一步我们可以设置一个专门的配置文件，并将参数按照某个事写入这个配置文件中，再在程序中解析这个配置文件获得编码的参数。如果参数不多的话，我们可以直接使用命令行将编码参数传入即可。

###(2)、按照要求初始化需要的FFMpeg结构

首先，所有涉及到编解码的的功能，都必须要注册音视频编解码器之后才能使用。注册编解码调用下面的函数：

	avcodec_register_all();

编解码器注册完成之后，根据指定的CODEC_ID查找指定的codec实例。CODEC_ID通常指定了编解码器的格式，在这里我们使用当前应用最为广泛的H.264格式为例。查找codec调用的函数为avcodec\_find_encoder，其声明格式为：

	AVCodec *avcodec_find_encoder(enum AVCodecID id);

该函数的输入参数为一个AVCodecID的枚举类型，返回值为一个指向AVCodec结构的指针，用于接收找到的编解码器实例。如果没有找到，那么该函数会返回一个空指针。调用方法如下：

	/* find the mpeg1 video encoder */
	ctx.codec = avcodec_find_encoder(AV_CODEC_ID_H264);	//根据CODEC_ID查找编解码器对象实例的指针
	if (!ctx.codec) 
	{
		fprintf(stderr, "Codec not found\n");
		return false;
	}

AVCodec查找成功后，下一步是分配AVCodecContext实例。分配AVCodecContext实例需要我们前面查找到的AVCodec作为参数，调用的是avcodec\_alloc_context3函数。其声明方式为：

	AVCodecContext *avcodec_alloc_context3(const AVCodec *codec);

其特点同avcodec\_find_encoder类似，返回一个指向AVCodecContext实例的指针。如果分配失败，会返回一个空指针。调用方式为：

	ctx.c = avcodec_alloc_context3(ctx.codec);			//分配AVCodecContext实例
	if (!ctx.c)
	{
		fprintf(stderr, "Could not allocate video codec context\n");
		return false;
	}

需注意，在分配成功之后，应将编码的参数设置赋值给AVCodecContext的成员。

现在，AVCodec、AVCodecContext的指针都已经分配好，然后以这两个对象的指针作为参数打开编码器对象。调用的函数为avcodec\_open2，声明方式为：

	int avcodec_open2(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options);

该函数的前两个参数是我们刚刚建立的两个对象，第三个参数为一个字典类型对象，用于保存函数执行过程总未能识别的AVCodecContext和另外一些私有设置选项。函数的返回值表示编码器是否打开成功，若成功返回0，失败返回一个负数。调用方式为：

	if (avcodec_open2(ctx.c, ctx.codec, NULL) < 0)		//根据编码器上下文打开编码器
	{
		fprintf(stderr, "Could not open codec\n");
		exit(1);
	}

然后，我们需要处理AVFrame对象。AVFrame表示视频原始像素数据的一个容器，处理该类型数据需要两个步骤，其一是分配AVFrame对象，其二是分配实际的像素数据的存储空间。分配对象空间类似于new操作符一样，只是需要调用函数av\_frame_alloc。如果失败，那么函数返回一个空指针。AVFrame对象分配成功后，需要设置图像的分辨率和像素格式等。实际调用过程如下：

	ctx.frame = av_frame_alloc();						//分配AVFrame对象
	if (!ctx.frame) 
	{
        fprintf(stderr, "Could not allocate video frame\n");
        return false;
    }
	ctx.frame->format = ctx.c->pix_fmt;
	ctx.frame->width = ctx.c->width;
	ctx.frame->height = ctx.c->height;

分配像素的存储空间需要调用av\_image_alloc函数，其声明方式为：

	int av_image_alloc(uint8_t *pointers[4], int linesizes[4], int w, int h, enum AVPixelFormat pix_fmt, int align);

该函数的四个参数分别表示AVFrame结构中的缓存指针、各个颜色分量的宽度、图像分辨率（宽、高）、像素格式和内存对其的大小。该函数会返回分配的内存的大小，如果失败则返回一个负值。具体调用方式如：

	ret = av_image_alloc(ctx.frame->data, ctx.frame->linesize, ctx.c->width, ctx.c->height, ctx.c->pix_fmt, 32);
	if (ret < 0) 
	{
		fprintf(stderr, "Could not allocate raw picture buffer\n");
		return false;
	}

###(3)、编码循环体

到此为止，我们的准备工作已经大致完成，下面开始执行实际编码的循环过程。用伪代码大致表示编码的流程为：

	while (numCoded < maxNumToCode)
	{
		read_yuv_data();
		encode_video_frame();
		write_out_h264();
	}

其中，read\_yuv_data部分直接使用fread语句读取即可，只需要知道的是，三个颜色分量Y/U/V的地址分别为AVframe::data[0]、AVframe::data[1]和AVframe::data[2]，图像的宽度分别为AVframe::linesize[0]、AVframe::linesize[1]和AVframe::linesize[2]。需要注意的是，linesize中的值通常指的是stride而不是width，也就是说，像素保存区可能是带有一定宽度的无效边区的，在读取数据时需注意。

编码前另外需要完成的操作时初始化AVPacket对象。该对象保存了编码之后的码流数据。对其进行初始化的操作非常简单，只需要调用av\_init_packet并传入AVPacket对象的指针。随后将AVPacket::data设为NULL，AVPacket::size赋值0.

成功将原始的YUV像素值保存到了AVframe结构中之后，便可以调用avcodec\_encode_video2函数进行实际的编码操作。该函数可谓是整个工程的核心所在，其声明方式为：

	int avcodec_encode_video2(AVCodecContext *avctx, AVPacket *avpkt, const AVFrame *frame, int *got_packet_ptr);

其参数和返回值的意义：

- avctx: AVCodecContext结构，指定了编码的一些参数；
- avpkt: AVPacket对象的指针，用于保存输出码流；
- frame：AVframe结构，用于传入原始的像素数据；
- got_packet_ptr：输出参数，用于标识AVPacket中是否已经有了完整的一帧；
- 返回值：编码是否成功。成功返回0，失败则返回负的错误码

通过输出参数*got_packet_ptr，我们可以判断是否应有一帧完整的码流数据包输出，如果是，那么可以将AVpacket中的码流数据输出出来，其地址为AVPacket::data，大小为AVPacket::size。具体调用方式如下：

	/* encode the image */
	ret = avcodec_encode_video2(ctx.c, &(ctx.pkt), ctx.frame, &got_output);	//将AVFrame中的像素信息编码为AVPacket中的码流
	if (ret < 0) 
	{
		fprintf(stderr, "Error encoding frame\n");
		exit(1);
	}

	if (got_output) 
	{
		//获得一个完整的编码帧
		printf("Write frame %3d (size=%5d)\n", frameIdx, ctx.pkt.size);
		fwrite(ctx.pkt.data, 1, ctx.pkt.size, io_param.pFout);
		av_packet_unref(&(ctx.pkt));
	}

因此，一个完整的编码循环提就可以使用下面的代码实现：

	/* encode 1 second of video */
	for (frameIdx = 0; frameIdx < io_param.nTotalFrames; frameIdx++)
	{
		av_init_packet(&(ctx.pkt));				//初始化AVPacket实例
		ctx.pkt.data = NULL;					// packet data will be allocated by the encoder
		ctx.pkt.size = 0;

		fflush(stdout);
				
		Read_yuv_data(ctx, io_param, 0);		//Y分量
		Read_yuv_data(ctx, io_param, 1);		//U分量
		Read_yuv_data(ctx, io_param, 2);		//V分量

		ctx.frame->pts = frameIdx;

		/* encode the image */
		ret = avcodec_encode_video2(ctx.c, &(ctx.pkt), ctx.frame, &got_output);	//将AVFrame中的像素信息编码为AVPacket中的码流
		if (ret < 0) 
		{
			fprintf(stderr, "Error encoding frame\n");
			exit(1);
		}

		if (got_output) 
		{
			//获得一个完整的编码帧
			printf("Write frame %3d (size=%5d)\n", frameIdx, ctx.pkt.size);
			fwrite(ctx.pkt.data, 1, ctx.pkt.size, io_param.pFout);
			av_packet_unref(&(ctx.pkt));
		}
	} //for (frameIdx = 0; frameIdx < io_param.nTotalFrames; frameIdx++)

###(4)、收尾处理

如果我们就此结束编码器的整个运行过程，我们会发现，编码完成之后的码流对比原来的数据少了一帧。这是因为我们是根据读取原始像素数据结束来判断循环结束的，这样最后一帧还保留在编码器中尚未输出。所以在关闭整个解码过程之前，我们必须继续执行编码的操作，直到将最后一帧输出为止。执行这项操作依然调用avcodec\_encode_video2函数，只是表示AVFrame的参数设为NULL即可：

	/* get the delayed frames */
	for (got_output = 1; got_output; frameIdx++) 
	{
		fflush(stdout);

		ret = avcodec_encode_video2(ctx.c, &(ctx.pkt), NULL, &got_output);		//输出编码器中剩余的码流
		if (ret < 0)
		{
			fprintf(stderr, "Error encoding frame\n");
			exit(1);
		}

		if (got_output) 
		{
			printf("Write frame %3d (size=%5d)\n", frameIdx, ctx.pkt.size);
			fwrite(ctx.pkt.data, 1, ctx.pkt.size, io_param.pFout);
			av_packet_unref(&(ctx.pkt));
		}
	} //for (got_output = 1; got_output; frameIdx++) 

此后，我们就可以按计划关闭编码器的各个组件，结束整个编码的流程。编码器组件的释放流程可类比建立流程，需要关闭AVCocec、释放AVCodecContext、释放AVFrame中的图像缓存和对象本身：

	avcodec_close(ctx.c);
	av_free(ctx.c);
	av_freep(&(ctx.frame->data[0]));
	av_frame_free(&(ctx.frame));

##3、总结

使用FFMpeg进行视频编码的主要流程如：

1. 首先解析、处理输入参数，如编码器的参数、图像的参数、输入输出文件；
2. 建立整个FFMpeg编码器的各种组件工具，顺序依次为：avcodec\_register\_all \-> avcodec\_find_encoder \-> avcodec\_alloc\_context3 \->  avcodec\_open2 \-> av\_frame\_alloc \->  av\_image\_alloc;
3. 编码循环：av\_init_packet \-> avcodec\_encode\_video2(两次) \-> av\_packet\_unref
4. 关闭编码器组件：avcodec\_close，av\_free，av\_freep，av\_frame\_free

---
#三、调用FFmpeg SDK对H.264格式的视频压缩码流进行解码

经过了上篇调用FFMpeg SDK对视频进行编码的过程之后，我们可以比较容易地理解本篇的内容，即上一篇的逆过程——将H.264格式的裸码流解码为像素格式的图像信息。

##1、FFMpeg视频解码器所包含的结构

同FFMpeg编码器类似，FFMpeg解码器也需要编码时的各种结构，除此之外，解码器还需要另一个结构——编解码解析器——用于从码流中截取出一帧完整的码流数据单元。因此我们定义一个编解码上下文结构为：

	/*************************************************
	Struct:			CodecCtx
	Description:	FFMpeg编解码器上下文
	*************************************************/
	typedef struct
	{
		AVCodec			*pCodec;				//编解码器实例指针
		AVCodecContext	*pCodecContext;			//编解码器上下文，指定了编解码的参数
		AVCodecParserContext *pCodecParserCtx;	//编解码解析器，从码流中截取完整的一个NAL Unit数据
	
		AVFrame			*frame;					//封装图像对象指针
		AVPacket		pkt;					//封装码流对象实例
	} CodecCtx;


##2、FFMpeg进行解码操作的主要步骤

###(1). 参数传递和解析

同编码器类似，解码器也需要传递参数。不过相比编码器，解码器在运行时所需要的大部分信息都包含在输入码流中，因此输入参数一般只需要指定一个待解码的视频码流文件即可

###(2). 按照要求初始化需要的FFMpeg结构

首先，所有涉及到编解码的的功能，都必须要注册音视频编解码器之后才能使用。注册编解码调用下面的函数：

	avcodec_register_all();

编解码器注册完成之后，根据指定的CODEC_ID查找指定的codec实例。CODEC_ID通常指定了编解码器的格式，在这里我们使用当前应用最为广泛的H.264格式为例。查找codec调用的函数为avcodec\_find_encoder，其声明格式为：

	AVCodec *avcodec_find_encoder(enum AVCodecID id);

该函数的输入参数为一个AVCodecID的枚举类型，返回值为一个指向AVCodec结构的指针，用于接收找到的编解码器实例。如果没有找到，那么该函数会返回一个空指针。调用方法如下：

	/* find the mpeg1 video encoder */
	ctx.codec = avcodec_find_encoder(AV_CODEC_ID_H264);	//根据CODEC_ID查找编解码器对象实例的指针
	if (!ctx.codec) 
	{
		fprintf(stderr, "Codec not found\n");
		return false;
	}

AVCodec查找成功后，下一步是分配AVCodecContext实例。分配AVCodecContext实例需要我们前面查找到的AVCodec作为参数，调用的是avcodec\_alloc_context3函数。其声明方式为：

	AVCodecContext *avcodec_alloc_context3(const AVCodec *codec);

其特点同avcodec\_find_encoder类似，返回一个指向AVCodecContext实例的指针。如果分配失败，会返回一个空指针。调用方式为：

	ctx.c = avcodec_alloc_context3(ctx.codec);			//分配AVCodecContext实例
	if (!ctx.c)
	{
		fprintf(stderr, "Could not allocate video codec context\n");
		return false;
	}

我们应该记得，在FFMpeg视频编码的实现中，AVCodecContext对象分配完成后，下一步实在该对象中设置编码的参数。而在解码器的实现中，基本不需要额外设置参数信息，因此这个对象更多地作为输出参数接收数据。因此对象分配完成后，不需要进一步的初始化操作。

解码器与编码器实现中不同的一点在于，解码器的实现中需要额外的一个AVCodecParserContext结构，用于从码流中截取一个完整的NAL单元。因此我们需要分配一个AVCodecParserContext类型的对象，使用函数av\_parser_init，声明为：

	AVCodecParserContext *av_parser_init(int codec_id);

调用方式为：

	ctx.pCodecParserCtx = av_parser_init(AV_CODEC_ID_H264);
	if (!ctx.pCodecParserCtx)
	{
		printf("Could not allocate video parser context\n");
		return false;
	}

随后，打开AVCodec对象，然后分配AVFrame对象：

	//打开AVCodec对象
	if (avcodec_open2(ctx.pCodecContext, ctx.pCodec, NULL) < 0)
	{
		fprintf(stderr, "Could not open codec\n");
		return false;
	}

	//分配AVFrame对象
	ctx.frame = av_frame_alloc();
	if (!ctx.frame) 
	{
		fprintf(stderr, "Could not allocate video frame\n");
		return false;
	}

###(3)、解码循环体

##3、总结

---
#四、调用FFmpeg SDK解析封装格式的视频为音频流和视频流
