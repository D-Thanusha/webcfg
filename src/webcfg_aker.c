/*
 * Copyright 2020 Comcast Cable Communications Management, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include "webcfg.h"
#include "webcfg_log.h"
#include "webcfg_aker.h"
#include "webcfg_generic.h"
#include "webcfg_event.h"
#include "webcfg_blob.h"
#include <wrp-c.h>
#include <wdmp-c.h>
#include <msgpack.h>
#include <libparodus.h>
#include <cJSON.h>
/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/
#define SERVICE_STATUS 		"service-status"
#define AKER_STATUS_ONLINE      "online"
#define WAIT_TIME_IN_SEC        30
#define CONTENT_TYPE_JSON       "application/json"
#define AKER_UPDATE_PARAM       "Device.DeviceInfo.X_RDKCENTRAL-COM_Aker.Update"
#define AKER_DELETE_PARAM       "Device.DeviceInfo.X_RDKCENTRAL-COM_Aker.Delete"
/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                            File Scoped Variables                           */
/*----------------------------------------------------------------------------*/
int akerDocVersion=0;
uint16_t akerTransId=0;
int wakeFlag = 0;
char *aker_status = NULL;
pthread_mutex_t client_mut=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t client_con=PTHREAD_COND_INITIALIZER;
/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/
static char *decodePayload(char *payload);
static void handleAkerStatus(int status, char *payload);
static void free_crud_message(wrp_msg_t *msg);
static char* parsePayloadForAkerStatus(char *payload);
static char *get_global_status();
static void set_global_status(char *status);
/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/

int send_aker_blob(char *paramName, char *blob, uint32_t blobSize, uint16_t docTransId, int version)
{
	WebcfgDebug("Aker blob is %s\n",blob);
	wrp_msg_t *msg = NULL;
	int ret = WDMP_FAILURE;
	char source[MAX_BUF_SIZE] = {'\0'};
	char destination[MAX_BUF_SIZE] = {'\0'};
	char trans_uuid[MAX_BUF_SIZE/4] = {'\0'};
	int sendStatus = -1;
	int retry_count = 0;
	int backoffRetryTime = 0;
	int c=2;

	if(paramName == NULL)
	{
		WebcfgError("aker paramName is NULL\n");
		return ret;
	}
	akerDocVersion = version;
	akerTransId = docTransId;

	msg = (wrp_msg_t *)malloc(sizeof(wrp_msg_t));
	if(msg != NULL)
	{
		memset(msg, 0, sizeof(wrp_msg_t));
		WebcfgDebug("Aker paramName is %s size %ld\n", paramName, sizeof(AKER_UPDATE_PARAM));
		if((strncmp(paramName, AKER_UPDATE_PARAM, sizeof(AKER_UPDATE_PARAM)) ==0) && (blobSize > 0))
		{
			msg->msg_type = WRP_MSG_TYPE__UPDATE;
			msg->u.crud.payload = blob;
			msg->u.crud.payload_size = blobSize;
		}
		else if(strncmp(paramName, AKER_DELETE_PARAM, sizeof(AKER_DELETE_PARAM)) ==0)
		{
			msg->msg_type = WRP_MSG_TYPE__DELETE;
			msg->u.crud.payload = NULL;
			msg->u.crud.payload_size = 0;
		}
		else //aker schedule RETRIEVE is not supported through webconfig.
		{
			WebcfgError("Invalid aker request\n");
			WEBCFG_FREE(msg);
			return ret;
		}
		snprintf(source, sizeof(source), "mac:%s/webcfg",get_deviceMAC());
		WebcfgDebug("source: %s\n",source);
		msg->u.crud.source = strdup(source);
		snprintf(destination, sizeof(destination), "mac:%s/aker/schedule",get_deviceMAC());
		WebcfgDebug("destination: %s\n",destination);
		msg->u.crud.dest = strdup(destination);
		snprintf(trans_uuid, sizeof(trans_uuid), "%hu", docTransId);
		msg->u.crud.transaction_uuid = strdup(trans_uuid);
		msg->u.crud.content_type = strdup(CONTENT_TYPE_JSON);

		while(retry_count<=3)
		{
		        backoffRetryTime = (int) pow(2, c) -1;

			sendStatus = libparodus_send(get_webcfg_instance(), msg);
			if(sendStatus == 0)
			{
				WebcfgInfo("Sent blob successfully to parodus\n");
				ret = WDMP_SUCCESS;
				retry_count = 0;
				break;
			}
			else
			{
				WebcfgError("Failed to send blob: '%s', retrying ...\n",libparodus_strerror(sendStatus));
				WebcfgInfo("send_aker_blob backoffRetryTime %d seconds\n", backoffRetryTime);
				sleep(backoffRetryTime);
				c++;
				retry_count++;
			}
		}
		free_crud_message(msg);
	}
	return ret;
}

//send aker-status upstream RETRIEVE request to parodus to check aker registration
WEBCFG_STATUS checkAkerStatus()
{
	wrp_msg_t *msg = NULL;
	WEBCFG_STATUS rv = WEBCFG_FAILURE;
	int sendStatus = -1;
	char *status_val = NULL;
	char source[MAX_BUF_SIZE/4] = {'\0'};
	char dest[MAX_BUF_SIZE/4] = {'\0'};
	char *transaction_uuid = NULL;

	msg = (wrp_msg_t *)malloc(sizeof(wrp_msg_t));
	if(msg != NULL)
	{
		memset(msg, 0, sizeof(wrp_msg_t));
		msg->msg_type = WRP_MSG_TYPE__RETREIVE;

		snprintf(source, sizeof(source), "mac:%s/webcfg",get_deviceMAC());
		WebcfgDebug("source: %s\n",source);
		msg->u.crud.source = strdup(source);
		snprintf(dest, sizeof(dest), "mac:%s/parodus/service-status/aker",get_deviceMAC());
		WebcfgDebug("dest: %s\n",dest);
		msg->u.crud.dest = strdup(dest);
		transaction_uuid = generate_trans_uuid();
		if(transaction_uuid !=NULL)
		{
			msg->u.crud.transaction_uuid = transaction_uuid;
			WebcfgInfo("transaction_uuid generated is %s\n", msg->u.crud.transaction_uuid);
		}
		msg->u.crud.content_type = strdup(CONTENT_TYPE_JSON);

		sendStatus = libparodus_send(get_webcfg_instance(), msg);
		if(sendStatus == 0)
		{
			WebcfgInfo("Sent aker retrieve request to parodus\n");
		}
		else
		{
			WebcfgError("Failed to send aker retrieve req: '%s'\n",libparodus_strerror(sendStatus));
		}

		//waiting to get response from parodus. add lock here while reading
		status_val = get_global_status();
		if(status_val !=NULL)
		{
			if (strcmp(status_val, AKER_STATUS_ONLINE) == 0)
			{
				WebcfgDebug("Received aker status as %s\n", status_val);
				rv = WEBCFG_SUCCESS;
			}
			else
			{
				WebcfgError("Received aker status as %s\n", status_val);
			}
			WEBCFG_FREE(status_val);
		}
		else
		{
			WebcfgError("Failed to get aker status\n");
		}
		wrp_free_struct (msg);
	}
	return rv;
}

/*----------------------------------------------------------------------------*/
/*                             Internal functions                             */
/*----------------------------------------------------------------------------*/
//set global aker status and to awake waiting getter threads
static void set_global_status(char *status)
{
	pthread_mutex_lock (&client_mut);
	WebcfgDebug("mutex lock in producer thread\n");
	wakeFlag = 1;
	aker_status = status;
	pthread_cond_signal(&client_con);
	pthread_mutex_unlock (&client_mut);
	WebcfgDebug("mutex unlock in producer thread\n");
}

//Combining getter func with pthread wait.
static char *get_global_status()
{
	char *temp = NULL;
	int  rv;
	struct timespec ts;
	pthread_mutex_lock (&client_mut);
	WebcfgDebug("mutex lock in consumer thread\n");
	WebcfgDebug("Before pthread cond wait in consumer thread\n");

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += WAIT_TIME_IN_SEC;

	while (!wakeFlag)
	{
		rv = pthread_cond_timedwait(&client_con, &client_mut, &ts);
		WebcfgDebug("After pthread_cond_timedwait\n");
		if (rv == ETIMEDOUT)
		{
			WebcfgError("Timeout Error. Unable to get service_status even after %d seconds\n", WAIT_TIME_IN_SEC);
			pthread_mutex_unlock(&client_mut);
			return NULL;
		}
	}
	temp = aker_status;
	wakeFlag = 0;
	pthread_mutex_unlock (&client_mut);
	WebcfgDebug("mutex unlock in consumer thread after cond wait\n");
	return temp;
}

static char *decodePayload(char *payload)
{
	msgpack_unpacked result;
	size_t off = 0;
	char *decodedPayload = NULL;
	msgpack_unpack_return ret;
	msgpack_unpacked_init(&result);
	ret = msgpack_unpack_next(&result, payload, strlen(payload), &off);
	if(ret == MSGPACK_UNPACK_SUCCESS)
	{
		msgpack_object obj = result.data;
		//msgpack_object_print(stdout, obj);
		//puts("");
		if (obj.type == MSGPACK_OBJECT_MAP) 
		{
			msgpack_object_map *map = &obj.via.map;
			msgpack_object_kv* p = map->ptr;
			msgpack_object *key = &p->key;
			msgpack_object *val = &p->val;
			if (0 == strncmp(key->via.str.ptr, "message", key->via.str.size))
			{
				if(val->via.str.ptr != NULL)
				{
					decodedPayload = strdup(val->via.str.ptr);
				}
			}
		}
	}
	else
	{
		WebcfgError("Failed to decode msgpack data\n");
	}
	msgpack_unpacked_destroy(&result);
	return decodedPayload;
}

static char* parsePayloadForAkerStatus(char *payload)
{
	cJSON *json = NULL;
	cJSON *akerStatusObj = NULL;
	char *aker_status_str = NULL;
	char *akerStatus = NULL;

	json = cJSON_Parse( payload );
	if( !json )
	{
		WebcfgError( "json parse error: [%s]\n", cJSON_GetErrorPtr() );
	}
	else
	{
		akerStatusObj = cJSON_GetObjectItem( json, SERVICE_STATUS );
		if( akerStatusObj != NULL)
		{
			aker_status_str = cJSON_GetObjectItem( json, SERVICE_STATUS )->valuestring;
			if ((aker_status_str != NULL) && strlen(aker_status_str) > 0)
			{
				akerStatus = strdup(aker_status_str);
				WebcfgDebug("akerStatus value parsed from payload is %s\n", akerStatus);
			}
			else
			{
				WebcfgError("aker status string is empty\n");
			}
		}
		else
		{
			WebcfgError("Failed to get akerStatus from payload\n");
		}
		cJSON_Delete(json);
	}
	return akerStatus;
}

static void handleAkerStatus(int status, char *payload)
{
	char data[MAX_BUF_SIZE] = {0};
	char *eventData = NULL;
	switch(status)
	{
		case 201:
		case 200:
			snprintf(data,sizeof(data),"aker,%hu,%u,ACK,%u",akerTransId,akerDocVersion,0);
		break;
		case 534:
		case 535:
			snprintf(data,sizeof(data),"aker,%hu,%u,NACK,%u,aker,%d,%s",akerTransId,akerDocVersion,0,status,payload);
		break;
		default:
			WebcfgError("Invalid status code %d\n",status);
			return;
	}
	WebcfgDebug("data: %s\n",data);
	eventData = data;
	webcfgCallback(eventData, NULL);
}

static void free_crud_message(wrp_msg_t *msg)
{
	if(msg)
	{
		if(msg->u.crud.source)
		{
			free(msg->u.crud.source);
		}
		if(msg->u.crud.dest)
		{
			free(msg->u.crud.dest);
		}
		if(msg->u.crud.transaction_uuid)
		{
			free(msg->u.crud.transaction_uuid);
		}
		if(msg->u.crud.content_type)
		{
			free(msg->u.crud.content_type);
		}
		free(msg);
	}
}

void processAkerUpdateDelete(wrp_msg_t *wrpMsg)
{
	char *sourceService, *sourceApplication =NULL;
	char *payload = NULL;

	sourceService = wrp_get_msg_element(WRP_ID_ELEMENT__SERVICE, wrpMsg, SOURCE);
	WebcfgDebug("sourceService: %s\n",sourceService);
	sourceApplication = wrp_get_msg_element(WRP_ID_ELEMENT__APPLICATION, wrpMsg, SOURCE);
	WebcfgDebug("sourceApplication: %s\n",sourceApplication);
	if(sourceService != NULL && sourceApplication != NULL && strcmp(sourceService,"aker")== 0 && strcmp(sourceApplication,"schedule")== 0)
	{
		WebcfgInfo("Response received from %s\n",sourceService);
		WEBCFG_FREE(sourceService);
		WEBCFG_FREE(sourceApplication);
		payload = decodePayload(wrpMsg->u.crud.payload);
		if(payload !=NULL)
		{
			WebcfgDebug("payload = %s\n",payload);
			WebcfgDebug("status: %d\n",wrpMsg->u.crud.status);
			handleAkerStatus(wrpMsg->u.crud.status, payload);
			WEBCFG_FREE(payload);
		}
		else
		{
			WebcfgError("decodePayload is NULL\n");
		}
	}
	wrp_free_struct (wrpMsg);

}

void processAkerRetrieve(wrp_msg_t *wrpMsg)
{
	char *sourceService, *sourceApplication =NULL;
	char *status=NULL;

	sourceService = wrp_get_msg_element(WRP_ID_ELEMENT__SERVICE, wrpMsg, SOURCE);
	sourceApplication = wrp_get_msg_element(WRP_ID_ELEMENT__APPLICATION, wrpMsg, SOURCE);
	WebcfgDebug("sourceService %s sourceApplication %s\n", sourceService, sourceApplication);
	if(sourceService != NULL && sourceApplication != NULL && strcmp(sourceService,"parodus")== 0 && strcmp(sourceApplication,"service-status/aker")== 0)
	{
		WebcfgDebug("Retrieve response received from parodus : %s transaction_uuid %s\n",(char *)wrpMsg->u.crud.payload, wrpMsg->u.crud.transaction_uuid );
		status = parsePayloadForAkerStatus(wrpMsg->u.crud.payload);
		if(status !=NULL)
		{
			//set this as global status. add lock before update it.
			set_global_status(status);
			WebcfgDebug("set aker-status value as %s\n", status);
		}
		WEBCFG_FREE(sourceService);
		WEBCFG_FREE(sourceApplication);
	}
	wrp_free_struct (wrpMsg);
}
