# FFmpeg_Tutorial
FFmpeg工具和sdk库的使用demo

一、使用FFmpeg命令行工具和批处理脚本进行简单的音视频文件编辑

1、基本介绍

对于每一个从事音视频技术开发的工程师，想必没有一个人对FFmpeg这个名称感到陌生。FFmpeg是一套非常知名的音视频处理的开源工具，它包含了开发完成的工具软件、封装好的函数库以及源代码供我们按需使用。FFmpeg提供了非常强大的功能，可以完成音视频的编码、解码、转码、视频采集、后处理（抓图、水印、封装/解封装、格式转换等），还有流媒体服务等诸多功能，可以说涵盖了音视频开发中绝大多数的领域。原生的FFmpeg是在Linux环境下开发的，但是通过各种方法（比如交叉编译等）可以使它运行在多种平台环境上，具有比较好的可移植性。

FFmpeg项目的官方网址为https://ffmpeg.org/    	
在它的官网上我们可以找到许多非常有用的内容，如项目的简介、版本更新日志、库和源代码的地址、使用文档等信息。官方的使用文档是我们在开发时必不可少的信息来源，其重要性不言而喻。除了官方网站以外，我们下载的FFmpeg的程序包中也有使用参考文档的离线版本。

2、FFmpeg组成

构成FFmpeg主要有三个部分，第一部分是四个作用不同的工具软件，分别是：ffmpeg.exe，ffplay.exe，ffserver.exe和ffprobe.exe。
- ffmpeg.exe：音视频转码、转换器
- ffplay.exe：简单的音视频播放器
- ffserver.exe：流媒体服务器
- ffprobe.exe：简单的多媒体码流分析器

第二部分是可以供开发者使用的SDK，为各个不同平台编译完成的库。如果说上面的四个工具软件都是完整成品形式的玩具，那么这些库就相当于乐高积木一样，我们可以根据自己的需求使用这些库开发自己的应用程序。这些库有：

	* libavcodec：包含音视频编码器和解码器
	* libavutil：包含多媒体应用常用的简化编程的工具，如随机数生成器、数据结构、数学函数等功能
	* libavformat：包含多种多媒体容器格式的封装、解封装工具
	* libavfilter：包含多媒体处理常用的滤镜功能
	* libavdevice：用于音视频数据采集和渲染等功能的设备相关
	* libswscale：用于图像缩放和色彩空间和像素格式转换功能
	* libswresample：用于音频重采样和格式转换等功能


第三部分是整个工程的源代码，无论是编译出来的可执行程序还是SDK，都是由这些源代码编译出来的。FFmpeg的源代码由C语言实现，主要在Linux平台上进行开发。FFmpeg不是一个孤立的工程，它还存在多个依赖的第三方工程来增强它自身的功能。在当前这一系列的博文/视频中，我们暂时不会涉及太多源代码相关的内容，主要以FFmpeg的工具和SDK的调用为主。到下一系列我们将专门研究如何编译源代码并根据源代码来进行二次开发。

3、FFMpeg工具的下载和使用

(1)FFmpeg工具的下载：
在官网上我们可以找到"Download"页面，该页上可以下载FFmpeg的工具、库和源代码等。在选择"Windows Packages"下的Windows Builds后，会跳转到Windows版本的下载页面：

在下载页面上，我们可以看到，对于32位和64位版本，分别提供了三种不同的模式：static、shared和dev
static: 该版本提供了静态版本的FFmpeg工具，将依赖的库生成在了最终的可执行文件中；作为工具而言此版本就可以满足我们的需求；
share: 该版本的工具包括可执行文件和dll，程序运行过程必须依赖于提供的dll文件；
dev: 提供了库的头文件和dll的引导库；

(2)ffplay.exe的使用
ffplay是一个极为简单的音视频媒体播放器。ffplay.exe使用了ffmpeg库和SDL库开发成的，可以用作FFmpeg API的测试工具。
ffplay的使用方法，最简单的是直接按照默认格式播放某一个音视频文件或流：
ffplay.exe  -i ../video/IMG_0886.MOV

除此之外，ffplay还支持传入各种参数来控制播放行为。比较常用的参数有：
-i input_file：输入文件名
-x width -y height：控制播放窗口的宽高
-t duration：控制播放的时长
-window_title title：播放窗口的标题，默认为输入文件名
-showmode mode：设置显示模式，0:显示视频;1:显示音频波形；2：显示音频频谱
-autoexit：设置视频播放完成后自动退出
其他参数可以参考官网的文档：https://www.ffmpeg.org/ffplay.html 或下载包里的文档

(3)ffprobe的使用
ffprobe可以提供简单的音视频文件分析功能。最简单的方法同ffplay类似：
ffprobe.exe  -i ../video/IMG_0886.MOV
分析完成后，ffprobe会显示音视频文件中包含的每个码流的信息，包括编码格式、像素分辨率、码率、帧率等信息，如下图所示：

(4)ffmpeg的使用
ffmpeg.exe可谓是整个工程的核心所在，它的主要功能是完成音视频各种各样的转换操作。
视频转码：ffmpeg.exe可以将视频文件由原格式转换为其他格式，如从avi转为mp4等：
ffmpeg -i ../video/IMG_0886.MOV ../video/output_mpeg4_mp3.avi 
这里，ffmpeg默认将视频编码格式选择为mpeg4，音频转码格式为mp3。如果我们希望保留原始编码，需要增加参数-c copy，表明不做任何转码操作：
ffmpeg -i ../video/IMG_0886.MOV -c copy ../video/output_copy.avi
如果我们希望将视频转换为其他编码格式，则需要在参数中指定目标格式-c:v libx265或-vcodec libx265。ffmpeg支持的所有编码器格式可以通过以下命令查看：
ffmpeg.exe -encoders
实际操作：
ffmpeg -i ../video/IMG_0886.MOV -c:v mjpeg  ../video/output_mjpeg.avi

视频解封装：ffmpeg可以将视频中的音频和视频流分别提取出来。需要在命令行中添加参数-an和-vn，分别表示屏蔽音频和视频流
@REM 提取视频流
ffmpeg -i ../video/IMG_0886.MOV -c:v copy -an ../video/IMG_0886_v.MOV
@REM 提取音频流
ffmpeg -i ../video/IMG_0886.MOV -c:a copy -vn ../video/IMG_0886_a.aac

视频截取：使用ffmpeg命令并指定参数-ss和-t，分别表示截取开始时刻和截取时长
@REM 视频截取
ffmpeg -ss 5 -t 5 -i ../video/IMG_0886.MOV -c copy ../video/IMG_0886_cut.MOV


二、调用FFmpeg SDK对YUV视频序列进行编码

三、调用FFmpeg SDK对H.264格式的视频压缩码流进行解码

四、调用FFmpeg SDK解析封装格式的视频为音频流和视频流
