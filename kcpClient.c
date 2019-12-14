#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "ikcp.c"

#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>

//=====================================================================
//
// kcp demo
//
// 说明：
// ikcp_input、ikcp_send返回值0 为正常
// 前期通信异常原因：
// 1.sendto(send->sockfd, buf, len, 0, (struct sockaddr *)&send->CientAddr,sizeof(struct sockaddr_in));
	// 参数buf 写成send->buff
// 2.ret = ikcp_send(send->pkcp, send->buff,sizeof(send->buff) );//第三个参数 正确应该是strlen(send->buff)+1
//   send->buff实际长度14字节，却传输512字节，后面全是0，kcp对这512字节数据全部进行封包，由UDP发送，导致kcp  input处理出错
//  send中的 buff[488]最大长度为488，传输正常，（488+24=512）[实际长度488+24kcp头部]，感觉kcp_input处理超过512的数据就出错
//=====================================================================

static int number = 0;

 typedef struct {
	unsigned char *ipstr;
	int port;
	
	ikcpcb *pkcp;
	
	int sockfd;
	struct sockaddr_in addr;//存放服务器的结构体
	
	char buff[488];//存放收发的消息
	
}kcpObj;


/* get system time */
void itimeofday(long *sec, long *usec)
{
	#if defined(__unix)
	struct timeval time;
	gettimeofday(&time, NULL);
	if (sec) *sec = time.tv_sec;
	if (usec) *usec = time.tv_usec;
	#else
	static long mode = 0, addsec = 0;
	BOOL retval;
	static IINT64 freq = 1;
	IINT64 qpc;
	if (mode == 0) {
		retval = QueryPerformanceFrequency((LARGE_INTEGER*)&freq);
		freq = (freq == 0)? 1 : freq;
		retval = QueryPerformanceCounter((LARGE_INTEGER*)&qpc);
		addsec = (long)time(NULL);
		addsec = addsec - (long)((qpc / freq) & 0x7fffffff);
		mode = 1;
	}
	retval = QueryPerformanceCounter((LARGE_INTEGER*)&qpc);
	retval = retval * 2;
	if (sec) *sec = (long)(qpc / freq) + addsec;
	if (usec) *usec = (long)((qpc % freq) * 1000000 / freq);
	#endif
}

/* get clock in millisecond 64 */
IINT64 iclock64(void)
{
	long s, u;
	IINT64 value;
	itimeofday(&s, &u);
	value = ((IINT64)s) * 1000 + (u / 1000);
	return value;
}

IUINT32 iclock()
{
	return (IUINT32)(iclock64() & 0xfffffffful);
}

/* sleep in millisecond */
void isleep(unsigned long millisecond)
{
	#ifdef __unix 	/* usleep( time * 1000 ); */
	struct timespec ts;
	ts.tv_sec = (time_t)(millisecond / 1000);
	ts.tv_nsec = (long)((millisecond % 1000) * 1000000);
	/*nanosleep(&ts, NULL);*/
	usleep((millisecond << 10) - (millisecond << 4) - (millisecond << 3));
	#elif defined(_WIN32)
	Sleep(millisecond);
	#endif
}



int udpOutPut(const char *buf, int len, ikcpcb *kcp, void *user){
   
 //  printf("使用udpOutPut发送数据\n");
   
    kcpObj *send = (kcpObj *)user;

	//发送信息
    int n = sendto(send->sockfd, buf, len, 0,(struct sockaddr *) &send->addr,sizeof(struct sockaddr_in));//【】
    if (n >= 0) 
	{       
		//会重复发送，因此牺牲带宽
		//printf("udpOutPut-send: 字节 =%d bytes   内容=[%s]\n", n ,buf+24);//24字节的KCP头部
        return n;
    } 
	else 
	{
        printf("udpOutPut: %d bytes send, error\n", n);
        return -1;
    }
}


int init(kcpObj *send)
{	
	send->sockfd = socket(AF_INET,SOCK_DGRAM,0);
	
	if(send->sockfd < 0)
	{
		perror("socket error！");
		exit(1);
	}
	
	bzero(&send->addr, sizeof(send->addr));
	
	//设置服务器ip、port
	send->addr.sin_family=AF_INET;
    send->addr.sin_addr.s_addr = inet_addr((char*)send->ipstr);
    send->addr.sin_port = htons(send->port);
	
	printf("sockfd = %d ip = %s  port = %d\n",send->sockfd,send->ipstr,send->port);
	
}

void loop(kcpObj *send)
{
	unsigned int len = sizeof(struct sockaddr_in);
	int n,ret;

	while(1)
	{
		isleep(1);
		
		//ikcp_update包含ikcp_flush，ikcp_flush将发送队列中的数据通过下层协议UDP进行发送
		ikcp_update(send->pkcp,iclock());//不是调用一次两次就起作用，要loop调用

		char buf[512]={0};

		//处理收消息
		n = recvfrom(send->sockfd,buf,512,MSG_DONTWAIT,(struct sockaddr *) &send->addr,&len);
	
		if(n < 0)//检测是否有UDP数据包
			continue;
			
	printf("UDP接收到数据包  大小= %d   buf =%s\n",n,buf+24);		
		
		//预接收数据:调用ikcp_input将裸数据交给KCP，这些数据有可能是KCP控制报文，并不是我们要的数据。 
		//kcp接收到下层协议UDP传进来的数据底层数据buffer转换成kcp的数据包格式
		ret = ikcp_input(send->pkcp, buf, n);	
		if(ret < 0)//检测ikcp_input是否提取到真正的数据
		{
			//printf("ikcp_input ret = %d\n",ret);
			continue;			
		}
		
//		ikcp_update(send->pkcp,iclock());//不是调用一次两次就起作用，要loop调用
		
		while(1)
		{	
			//kcp将接收到的kcp数据包还原成之前kcp发送的buffer数据		
			ret = ikcp_recv(send->pkcp, buf, n);		
			if(ret < 0)//检测ikcp_recv提取到的数据	
			{
				//printf("ikcp_recv ret = %d\n",ret);
				break;
			}
		}
		
		printf("数据交互  ip = %s  port = %d\n",inet_ntoa(send->addr.sin_addr),ntohs(send->addr.sin_port));	
		
		if(strcmp(buf,"Conn-OK") == 0)
		{
			//kcp收到确认连接包，则进行交互
			printf("Data from Server-> %s\n",buf);	
			//把要发送的buffer分片成KCP的数据包格式，插入待发送队列中。
			ret = ikcp_send(send->pkcp, send->buff,sizeof(send->buff) );//strlen(send->buff)+1
			printf("Client reply -> 内容[%s] 字节[%d]  ret = %d\n",send->buff,(int)sizeof(send->buff),ret);//发送成功的
			number++;
			printf("第[%d]次发\n",number);					
		}
		
		//发消息
		if(strcmp(buf,"Server:Hello!") == 0)
		{		
			//kcp收到确认连接包，则进行交互
			printf("Data from Server-> %s\n",buf);
			
			//kcp收到交互包，则回复	
			//把要发送的buffer分片成KCP的数据包格式，插入待发送队列中。
			ikcp_send(send->pkcp, send->buff, sizeof(send->buff));				
			number++;		
			printf("第[%d]次发\n",number);
		}	
	}
	
}

int main(int argc,char *argv[])
{
	//printf("this is kcpClient,请输入服务器 ip地址和端口号：\n");
	if(argc != 3)
	{
		printf("请输入服务器ip地址和端口号\n");
		return -1;
	}
	printf("this is kcpClient\n");
	
	unsigned char *ipstr = (unsigned char *)argv[1];
	unsigned char *port  = (unsigned char *)argv[2];
	
	kcpObj send;
	send.ipstr = ipstr;
	send.port = atoi(argv[2]);
	
	init(&send);//初始化send,主要是设置与服务器通信的套接字对象
	
	bzero(send.buff,sizeof(send.buff));
	char Msg[] = "Client:Hello!";//与服务器后续交互	
	memcpy(send.buff,Msg,sizeof(Msg));
	
	ikcpcb *kcp = ikcp_create(0x1, (void *)&send);//创建kcp对象把send传给kcp的user变量
	kcp->output = udpOutPut;//设置kcp对象的回调函数
	ikcp_nodelay(kcp,0, 10, 0, 0);//(kcp1, 0, 10, 0, 0); 1, 10, 2, 1
	ikcp_wndsize(kcp, 128, 128);
	
	send.pkcp = kcp;
	
	char temp[] = "Conn";//与服务器初次通信		
	int	ret = ikcp_send(send.pkcp,temp,(int)sizeof(temp)); 
		printf("ikcp_send发送连接请求： [%s] len=%d ret = %d\n",temp,(int)sizeof(temp),ret);//发送成功的
		

	loop(&send);//循环处理
	
	return 0;	
}