#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/queue.h"

int queue_init(queue_t **q, int capacity, int size)
{
	*q = malloc(sizeof(queue_t));//开辟队列结构的空间
	if(*q == NULL)//判断开辟队列结构的空间是否失败
		return -1;//由于开辟队列结构的空间失败,结束函数,并且返回-1
	(*q)->s = calloc(capacity + 1, size);//开辟队列的空间
	if((*q)->s == NULL)//判断开辟队列的空间是否失败
	{
		free(*q);//释放队列结构的空间
		*q = NULL;//避免调用者的指针变成野指针
		return -2;//由于开辟队列的空间失败,结束函数,并且返回-2
	}
	(*q)->front = (*q)->rear = 0;//将队头和队尾标记在0的位置
	(*q)->capacity = capacity + 1;//在队列结构中存储客户指定的容量 + 1
	(*q)->size = size;//在队列结构中存储客户指定的一个空间的大小
	
	return 0;
}

int queue_is_empty(const queue_t *q)
{
	return q->front == q->rear;
}

int queue_is_full(const queue_t *q)
{
	return ((q->rear + 1) % q->capacity) == q->front;
}

int queue_en(queue_t *q, const void *data)
{
	if(queue_is_full(q))//判断队列是否满了
		return -1;//由于队列满了,结束函数,并且返回-1
	q->rear = (q->rear + 1) % q->capacity;//移动队尾标记
	memcpy((char *)q->s + q->rear * q->size, data, q->size);//入队数据

	return 0;
}

int queue_de(queue_t *q, void *data)
{
	if(queue_is_empty(q))//判断队列是否空了
		return -1;//由于队列空了,结束函数,并且返回-1
	q->front = (q->front + 1) % q->capacity;//移动队头标记
	memcpy(data, (char *)q->s + q->front * q->size, q->size);//出队数据
	memset((char *)q->s + q->front * q->size, '\0', q->size);//清空脏数据

	return 0;
}

void queue_destroy(queue_t *q)
{
	free(q->s);//先释放队列的空间
	free(q);//再释放队列结构的空间
}