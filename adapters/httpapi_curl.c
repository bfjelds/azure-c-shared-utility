// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>

#include "azure_c_shared_utility/strings.h"
#include "azure_c_shared_utility/httpapi.h"
#include "azure_c_shared_utility/httpheaders.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "curl/curl.h"
#include <openssl/x509_vfy.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/bio.h>
#include "azure_c_shared_utility/xlogging.h"

#define TEMP_BUFFER_SIZE 1024

DEFINE_ENUM_STRINGS(HTTPAPI_RESULT, HTTPAPI_RESULT_VALUES);

typedef struct HTTP_HANDLE_DATA_TAG
{
    CURL* curl;
    char* hostURL;
    long timeout;
    long lowSpeedLimit;
    long lowSpeedTime;
    long forbidReuse;
    long freshConnect;
    long verbose;
    const char* x509privatekey;
    const char* x509certificate;
} HTTP_HANDLE_DATA;

typedef struct HTTP_RESPONSE_CONTENT_BUFFER_TAG
{
    unsigned char* buffer;
    size_t bufferSize;
    unsigned char error;
} HTTP_RESPONSE_CONTENT_BUFFER;

static size_t nUsersOfHTTPAPI = 0; /*used for reference counting (a weak one)*/

HTTPAPI_RESULT HTTPAPI_Init(void)
{
    HTTPAPI_RESULT result;
    if (nUsersOfHTTPAPI == 0)
    {
        if (curl_global_init(CURL_GLOBAL_NOTHING) != 0)
        {
            result = HTTPAPI_INIT_FAILED;
            LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
        }
        else
        {
            nUsersOfHTTPAPI++;
            result = HTTPAPI_OK;
        }
    }
    else
    {
        nUsersOfHTTPAPI++;
        result = HTTPAPI_OK;
    }

    return result;
}

void HTTPAPI_Deinit(void)
{
    if (nUsersOfHTTPAPI > 0)
    {
        nUsersOfHTTPAPI--;
        if (nUsersOfHTTPAPI == 0)
        {
            curl_global_cleanup();
        }
    }
}

HTTP_HANDLE HTTPAPI_CreateConnection(const char* hostName)
{
    HTTP_HANDLE_DATA* httpHandleData;

    if (hostName == NULL)
    {
        LogError("invalid arg const char* hostName = %p", hostName);
        httpHandleData = NULL;
    }
    else
    {
        httpHandleData = (HTTP_HANDLE_DATA*)malloc(sizeof(HTTP_HANDLE_DATA));
        if (httpHandleData != NULL)
        {
            size_t hostURL_size = strlen("https://") + strlen(hostName) + 1;
            httpHandleData->hostURL = malloc(hostURL_size);
            if (httpHandleData->hostURL == NULL)
            {
                LogError("unable to malloc");
                free(httpHandleData);
                httpHandleData = NULL;
            }
            else
            {
                if ((strcpy_s(httpHandleData->hostURL, hostURL_size, "https://") == 0) &&
                    (strcat_s(httpHandleData->hostURL, hostURL_size, hostName) == 0))
                {
                    httpHandleData->curl = curl_easy_init();
                    if (httpHandleData->curl == NULL)
                    {
                        free(httpHandleData->hostURL);
                        free(httpHandleData);
                        httpHandleData = NULL;
                    }
                    else
                    {
                        httpHandleData->timeout = 242 * 1000; /*242 seconds seems like a nice enough time. Reasone for 242:
                                                                1. http://curl.haxx.se/libcurl/c/CURLOPT_TIMEOUT.html says Normally, name lookups can take a considerable time and limiting operations to less than a few minutes risk aborting perfectly normal operations.
                                                                2. 256KB of data... at 9600 bps transfers in about 218 seconds. Add to that a buffer of 10%... round it up to 242 :)*/
                        httpHandleData->lowSpeedTime = 0;
                        httpHandleData->lowSpeedLimit = 0;
                        httpHandleData->forbidReuse = 0;
                        httpHandleData->freshConnect = 0;
                        httpHandleData->verbose = 0;
                        httpHandleData->x509certificate = NULL;
                        httpHandleData->x509privatekey = NULL;
                    }
                }
                else
                {
                    free(httpHandleData->hostURL);
                    free(httpHandleData);
                    httpHandleData = NULL;
                }
            }
        }
    }

    return (HTTP_HANDLE)httpHandleData;
}

void HTTPAPI_CloseConnection(HTTP_HANDLE handle)
{
    HTTP_HANDLE_DATA* httpHandleData = (HTTP_HANDLE_DATA*)handle;
    if (httpHandleData != NULL)
    {
        free(httpHandleData->hostURL);
        curl_easy_cleanup(httpHandleData->curl);
        free(httpHandleData);
    }
}

static size_t HeadersWriteFunction(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    HTTP_HEADERS_HANDLE responseHeadersHandle = (HTTP_HEADERS_HANDLE)userdata;
    char* headerLine = (char*)ptr;

    if (headerLine != NULL)
    {
        char* token = strtok(headerLine, "\r\n");
        while ((token != NULL) &&
               (token[0] != '\0'))
        {
            char* whereIsColon = strchr(token, ':');
            if(whereIsColon!=NULL)
            {
                *whereIsColon='\0';
                HTTPHeaders_AddHeaderNameValuePair(responseHeadersHandle, token, whereIsColon+1);
                *whereIsColon=':';
            }
            else
            {
                /*not a header, maybe a status-line*/
            }
            token = strtok(NULL, "\r\n");
        }
    }

    return size * nmemb;
}

static size_t ContentWriteFunction(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    HTTP_RESPONSE_CONTENT_BUFFER* responseContentBuffer = (HTTP_RESPONSE_CONTENT_BUFFER*)userdata;
    if ((userdata != NULL) &&
        (ptr != NULL) &&
        (size * nmemb > 0))
    {
        void* newBuffer = realloc(responseContentBuffer->buffer, responseContentBuffer->bufferSize + (size * nmemb));
        if (newBuffer != NULL)
        {
            responseContentBuffer->buffer = newBuffer;
            memcpy(responseContentBuffer->buffer + responseContentBuffer->bufferSize, ptr, size * nmemb);
            responseContentBuffer->bufferSize += size * nmemb;
        }
        else
        {
            LogError("Could not allocate buffer of size %zu", (size_t)(responseContentBuffer->bufferSize + (size * nmemb)));
            responseContentBuffer->error = 1;
            if (responseContentBuffer->buffer != NULL)
            {
                free(responseContentBuffer->buffer);
            }
        }
    }

    return size * nmemb;
}

static CURLcode ssl_ctx_callback(CURL *curl, void *ssl_ctx, void *userptr)
{
    CURLcode result;
    
    if (
        (curl == NULL) ||
        (ssl_ctx == NULL) ||
        (userptr == NULL)
        )
    {
        LogError("unexpected parameter CURL *curl=%p, void *ssl_ctx=%p, void *userptr=%p", curl, ssl_ctx, userptr);
        result = CURLE_SSL_CERTPROBLEM;
    }
    else
    {
        HTTP_HANDLE_DATA *httpHandleData = (HTTP_HANDLE_DATA *)userptr;
        BIO *bio_certificate;
        bio_certificate = BIO_new_mem_buf(httpHandleData->x509certificate, -1);
        if (bio_certificate == NULL)
        {
            LogError("cannot create  BIO *bio_certificate");
            result = CURLE_OUT_OF_MEMORY;
        }
        else
        {
            X509 *cert = PEM_read_bio_X509(bio_certificate, NULL, 0, NULL);
            if (cert == NULL)
            {
                LogError("cannot create X509 *cert");
                result = CURLE_SSL_CERTPROBLEM;
            }
            else
            {
                BIO *bio_privatekey;
                bio_privatekey = BIO_new_mem_buf(httpHandleData->x509privatekey, -1);
                if (bio_privatekey == NULL)
                {
                    LogError("cannot create BIO *bio_privatekey;");
                    result = CURLE_OUT_OF_MEMORY;
                }
                else
                {
                    RSA* privatekey = PEM_read_bio_RSAPrivateKey(bio_privatekey, NULL, 0, NULL);
                    if (privatekey == NULL)
                    {
                        LogError("cannot create RSA* privatekey");
                        result = CURLE_SSL_CERTPROBLEM; /*there's no better code in CURL about not being able to load a private key*/
                    }
                    else
                    {
                        if (SSL_CTX_use_certificate((SSL_CTX*)ssl_ctx, cert) != 1)
                        {
                            LogError("cannot SSL_CTX_use_certificate");
                            result = CURLE_SSL_CERTPROBLEM; /*there's no better code in CURL about not being able to SSL_CTX_use_certificate*/
                        }
                        else
                        {
                            if (SSL_CTX_use_RSAPrivateKey(ssl_ctx, privatekey) != 1)
                            {
                                LogError("cannot SSL_CTX_use_RSAPrivateKey");
                                result = CURLE_SSL_CERTPROBLEM; /*there's no better code in CURL about not being able to put a private key in an SSL context*/
                            }
                            else
                            {
                                result = CURLE_OK;
                            }
                        }
                        RSA_free(privatekey);
                    }
                    BIO_free(bio_privatekey);
                }
                X509_free(cert);
            }
            BIO_free(bio_certificate);
        }
    }
    return result;
}

HTTPAPI_RESULT HTTPAPI_ExecuteRequest(HTTP_HANDLE handle, HTTPAPI_REQUEST_TYPE requestType, const char* relativePath,
                                      HTTP_HEADERS_HANDLE httpHeadersHandle, const unsigned char* content,
                                      size_t contentLength, unsigned int* statusCode,
                                      HTTP_HEADERS_HANDLE responseHeadersHandle, BUFFER_HANDLE responseContent)
{
    HTTPAPI_RESULT result;
    HTTP_HANDLE_DATA* httpHandleData = (HTTP_HANDLE_DATA*)handle;
    size_t headersCount;
    HTTP_RESPONSE_CONTENT_BUFFER responseContentBuffer;

    if ((httpHandleData == NULL) ||
        (relativePath == NULL) ||
        (httpHeadersHandle == NULL) ||
        ((content == NULL) && (contentLength > 0))
    )
    {
        result = HTTPAPI_INVALID_ARG;
        LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
    }
    else if (HTTPHeaders_GetHeaderCount(httpHeadersHandle, &headersCount) != HTTP_HEADERS_OK)
    {
        result = HTTPAPI_INVALID_ARG;
        LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
    }
    else
    {
        char* tempHostURL;
        size_t tempHostURL_size = strlen(httpHandleData->hostURL) + strlen(relativePath) + 1;
        tempHostURL = malloc(tempHostURL_size);
        if (tempHostURL == NULL)
        {
            result = HTTPAPI_ERROR;
            LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
        }
        else
        {
            if (curl_easy_setopt(httpHandleData->curl, CURLOPT_VERBOSE, httpHandleData->verbose) != CURLE_OK)
            {
                result = HTTPAPI_SET_OPTION_FAILED;
                LogError("failed to set CURLOPT_VERBOSE (result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
            }
            else if ((strcpy_s(tempHostURL, tempHostURL_size, httpHandleData->hostURL) != 0) ||
                (strcat_s(tempHostURL, tempHostURL_size, relativePath) != 0))
            {
                result = HTTPAPI_STRING_PROCESSING_ERROR;
                LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
            }
            /* set the URL */
            else if (curl_easy_setopt(httpHandleData->curl, CURLOPT_URL, tempHostURL) != CURLE_OK)
            {
                result = HTTPAPI_SET_OPTION_FAILED;
                LogError("failed to set CURLOPT_URL (result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
            }
            else if (curl_easy_setopt(httpHandleData->curl, CURLOPT_TIMEOUT_MS, httpHandleData->timeout) != CURLE_OK)
            {
                result = HTTPAPI_SET_OPTION_FAILED;
                LogError("failed to set CURLOPT_TIMEOUT_MS (result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
            }
            else if (curl_easy_setopt(httpHandleData->curl, CURLOPT_LOW_SPEED_LIMIT, httpHandleData->lowSpeedLimit) != CURLE_OK)
            {
                result = HTTPAPI_SET_OPTION_FAILED;
                LogError("failed to set CURLOPT_LOW_SPEED_LIMIT (result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
            }
            else if (curl_easy_setopt(httpHandleData->curl, CURLOPT_LOW_SPEED_TIME, httpHandleData->lowSpeedTime) != CURLE_OK)
            {
                result = HTTPAPI_SET_OPTION_FAILED;
                LogError("failed to set CURLOPT_LOW_SPEED_TIME (result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
            }
            else if (curl_easy_setopt(httpHandleData->curl, CURLOPT_FRESH_CONNECT, httpHandleData->freshConnect) != CURLE_OK)
            {
                result = HTTPAPI_SET_OPTION_FAILED;
                LogError("failed to set CURLOPT_FRESH_CONNECT (result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
            }
            else if (curl_easy_setopt(httpHandleData->curl, CURLOPT_FORBID_REUSE, httpHandleData->forbidReuse) != CURLE_OK)
            {
                result = HTTPAPI_SET_OPTION_FAILED;
                LogError("failed to set CURLOPT_FORBID_REUSE (result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
            }
            else if (curl_easy_setopt(httpHandleData->curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1) != CURLE_OK)
            {
                result = HTTPAPI_SET_OPTION_FAILED;
                LogError("failed to set CURLOPT_HTTP_VERSION (result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
            }
            else
            {
                result = HTTPAPI_OK;

                switch (requestType)
                {
                default:
                    result = HTTPAPI_INVALID_ARG;
                    LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                    break;

                case HTTPAPI_REQUEST_GET:
                    if (curl_easy_setopt(httpHandleData->curl, CURLOPT_HTTPGET, 1L) != CURLE_OK)
                    {
                        result = HTTPAPI_SET_OPTION_FAILED;
                        LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                    }
                    else
                    {
                        if (curl_easy_setopt(httpHandleData->curl, CURLOPT_CUSTOMREQUEST, NULL) != CURLE_OK)
                        {
                            result = HTTPAPI_SET_OPTION_FAILED;
                            LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                        }
                    }

                    break;

                case HTTPAPI_REQUEST_POST:
                    if (curl_easy_setopt(httpHandleData->curl, CURLOPT_POST, 1L) != CURLE_OK)
                    {
                        result = HTTPAPI_SET_OPTION_FAILED;
                        LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                    }
                    else
                    {
                        if (curl_easy_setopt(httpHandleData->curl, CURLOPT_CUSTOMREQUEST, NULL) != CURLE_OK)
                        {
                            result = HTTPAPI_SET_OPTION_FAILED;
                            LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                        }
                    }

                    break;

                case HTTPAPI_REQUEST_PUT:
                    if (curl_easy_setopt(httpHandleData->curl, CURLOPT_POST, 1L))
                    {
                        result = HTTPAPI_SET_OPTION_FAILED;
                        LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                    }
                    else
                    {
                        if (curl_easy_setopt(httpHandleData->curl, CURLOPT_CUSTOMREQUEST, "PUT") != CURLE_OK)
                        {
                            result = HTTPAPI_SET_OPTION_FAILED;
                            LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                        }
                    }
                    break;

                case HTTPAPI_REQUEST_DELETE:
                    if (curl_easy_setopt(httpHandleData->curl, CURLOPT_POST, 1L) != CURLE_OK)
                    {
                        result = HTTPAPI_SET_OPTION_FAILED;
                        LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                    }
                    else
                    {
                        if (curl_easy_setopt(httpHandleData->curl, CURLOPT_CUSTOMREQUEST, "DELETE") != CURLE_OK)
                        {
                            result = HTTPAPI_SET_OPTION_FAILED;
                            LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                        }
                    }
                    break;

                case HTTPAPI_REQUEST_PATCH:
                    if (curl_easy_setopt(httpHandleData->curl, CURLOPT_POST, 1L) != CURLE_OK)
                    {
                        result = HTTPAPI_SET_OPTION_FAILED;
                        LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                    }
                    else
                    {
                        if (curl_easy_setopt(httpHandleData->curl, CURLOPT_CUSTOMREQUEST, "PATCH") != CURLE_OK)
                        {
                            result = HTTPAPI_SET_OPTION_FAILED;
                            LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                        }
                    }

                    break;
                }

                if (result == HTTPAPI_OK)
                {
                    /* add headers */
                    struct curl_slist* headers = NULL;
                    size_t i;

                    for (i = 0; i < headersCount; i++)
                    {
                        char *tempBuffer;
                        if (HTTPHeaders_GetHeader(httpHeadersHandle, i, &tempBuffer) != HTTP_HEADERS_OK)
                        {
                            /* error */
                            result = HTTPAPI_HTTP_HEADERS_FAILED;
                            LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                            break;
                        }
                        else
                        {
                            struct curl_slist* newHeaders = curl_slist_append(headers, tempBuffer);
                            if (newHeaders == NULL)
                            {
                                result = HTTPAPI_ALLOC_FAILED;
                                LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                                free(tempBuffer);
                                break;
                            }
                            else
                            {
                                free(tempBuffer);
                                headers = newHeaders;
                            }
                        }
                    }

                    if (result == HTTPAPI_OK)
                    {
                        if (curl_easy_setopt(httpHandleData->curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK)
                        {
                            result = HTTPAPI_SET_OPTION_FAILED;
                            LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                        }
                        else
                        {
                            /* add content */
                            if ((content != NULL) &&
                                (contentLength > 0))
                            {
                                if ((curl_easy_setopt(httpHandleData->curl, CURLOPT_POSTFIELDS, (void*)content) != CURLE_OK) ||
                                    (curl_easy_setopt(httpHandleData->curl, CURLOPT_POSTFIELDSIZE, contentLength) != CURLE_OK))
                                {
                                    result = HTTPAPI_SET_OPTION_FAILED;
                                    LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                                }
                            }
                            else
                            {
                                if (requestType != HTTPAPI_REQUEST_GET)
                                {
                                    if ((curl_easy_setopt(httpHandleData->curl, CURLOPT_POSTFIELDS, (void*)NULL) != CURLE_OK) ||
                                        (curl_easy_setopt(httpHandleData->curl, CURLOPT_POSTFIELDSIZE, 0) != CURLE_OK))
                                    {
                                        result = HTTPAPI_SET_OPTION_FAILED;
                                        LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                                    }
                                }
                                else
                                {
                                    /*GET request cannot POST, so "do nothing*/
                                }
                            }

                            if (result == HTTPAPI_OK)
                            {
                                if ((curl_easy_setopt(httpHandleData->curl, CURLOPT_WRITEHEADER, NULL) != CURLE_OK) ||
                                    (curl_easy_setopt(httpHandleData->curl, CURLOPT_HEADERFUNCTION, NULL) != CURLE_OK) ||
                                    (curl_easy_setopt(httpHandleData->curl, CURLOPT_WRITEFUNCTION, ContentWriteFunction) != CURLE_OK))
                                {
                                    result = HTTPAPI_SET_OPTION_FAILED;
                                    LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                                }
                                else
                                {
                                    if (responseHeadersHandle != NULL)
                                    {
                                        /* setup the code to get the response headers */
                                        if ((curl_easy_setopt(httpHandleData->curl, CURLOPT_WRITEHEADER, responseHeadersHandle) != CURLE_OK) ||
                                            (curl_easy_setopt(httpHandleData->curl, CURLOPT_HEADERFUNCTION, HeadersWriteFunction) != CURLE_OK))
                                        {
                                            result = HTTPAPI_SET_OPTION_FAILED;
                                            LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                                        }
                                    }

                                    if (result == HTTPAPI_OK)
                                    {
                                        responseContentBuffer.buffer = NULL;
                                        responseContentBuffer.bufferSize = 0;
                                        responseContentBuffer.error = 0;

                                        if (curl_easy_setopt(httpHandleData->curl, CURLOPT_WRITEDATA, &responseContentBuffer) != CURLE_OK)
                                        {
                                            result = HTTPAPI_SET_OPTION_FAILED;
                                            LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                                        }

                                        if (result == HTTPAPI_OK)
                                        {
                                            /* Execute request */
                                            CURLcode curlRes = curl_easy_perform(httpHandleData->curl);
                                            if (curlRes != CURLE_OK)
                                            {
                                                LogError("curl_easy_perform() failed: %s\n", curl_easy_strerror(curlRes));
                                                result = HTTPAPI_OPEN_REQUEST_FAILED;
                                                LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                                            }
                                            else
                                            {
                                                long httpCode;

                                                /* get the status code */
                                                if (curl_easy_getinfo(httpHandleData->curl, CURLINFO_RESPONSE_CODE, &httpCode) != CURLE_OK)
                                                {
                                                    result = HTTPAPI_QUERY_HEADERS_FAILED;
                                                    LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                                                }
                                                else if (responseContentBuffer.error)
                                                {
                                                    result = HTTPAPI_READ_DATA_FAILED;
                                                    LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                                                }
                                                else
                                                {
                                                    if (statusCode != NULL)
                                                    {
                                                        *statusCode = httpCode;
                                                    }

                                                    /* fill response content length */
                                                    if (responseContent != NULL)
                                                    {
                                                        if ((responseContentBuffer.bufferSize > 0) && (BUFFER_build(responseContent, responseContentBuffer.buffer, responseContentBuffer.bufferSize) != 0))
                                                        {
                                                            result = HTTPAPI_INSUFFICIENT_RESPONSE_BUFFER;
                                                            LogError("(result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
                                                        }
                                                        else
                                                        {
                                                            /*all nice*/
                                                        }
                                                    }

                                                    if (httpCode >= 300)
                                                    {
                                                        LogError("Failure in HTTP communication: server reply code is %ld", httpCode);
                                                        LogInfo("HTTP Response:%*.*s", (int)responseContentBuffer.bufferSize,
                                                            (int)responseContentBuffer.bufferSize, responseContentBuffer.buffer);
                                                    }
                                                    else
                                                    {
                                                        result = HTTPAPI_OK;
                                                    }
                                                }
                                            }
                                        }

                                        if (responseContentBuffer.buffer != NULL)
                                        {
                                            free(responseContentBuffer.buffer);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    curl_slist_free_all(headers);
                }
            }
            free(tempHostURL);
        }
    }

    return result;
}

HTTPAPI_RESULT HTTPAPI_SetOption(HTTP_HANDLE handle, const char* optionName, const void* value)
{
    HTTPAPI_RESULT result;
    if (
        (handle == NULL) ||
        (optionName == NULL) ||
        (value == NULL)
        )
    {
        result = HTTPAPI_INVALID_ARG;
        LogError("invalid parameter (NULL) passed to HTTPAPI_SetOption");
    }
    else
    {
        HTTP_HANDLE_DATA* httpHandleData = (HTTP_HANDLE_DATA*)handle;
        if (strcmp("timeout", optionName) == 0)
        {
            long timeout = (long)(*(unsigned int*)value);
            httpHandleData->timeout = timeout;
            result = HTTPAPI_OK;
        }
        else if (strcmp("CURLOPT_LOW_SPEED_LIMIT", optionName) == 0)
        {
            httpHandleData->lowSpeedLimit = *(const long*)value;
            result = HTTPAPI_OK;
        }
        else if (strcmp("CURLOPT_LOW_SPEED_TIME", optionName) == 0)
        {
            httpHandleData->lowSpeedTime = *(const long*)value; 
            result = HTTPAPI_OK;
        }
        else if (strcmp("CURLOPT_FRESH_CONNECT", optionName) == 0)
        {
            httpHandleData->freshConnect = *(const long*)value;
            result = HTTPAPI_OK;
        }
        else if (strcmp("CURLOPT_FORBID_REUSE", optionName) == 0)
        {
            httpHandleData->forbidReuse = *(const long*)value;
            result = HTTPAPI_OK;
        }
        else if (strcmp("CURLOPT_VERBOSE", optionName) == 0)
        {
            httpHandleData->verbose = *(const long*)value;
            result = HTTPAPI_OK;
        }
        else if (strcmp("x509privatekey", optionName) == 0)
        {
            httpHandleData->x509privatekey = value;
            if (httpHandleData->x509certificate != NULL)
            {
                if (curl_easy_setopt(httpHandleData->curl, CURLOPT_SSL_CTX_FUNCTION, ssl_ctx_callback) != CURLE_OK)
                {
                    LogError("unable to curl_easy_setopt");
                    result = HTTPAPI_ERROR;
                }
                else
                {
                    if (curl_easy_setopt(httpHandleData->curl, CURLOPT_SSL_CTX_DATA, httpHandleData) != CURLE_OK)
                    {
                        LogError("unable to curl_easy_setopt");
                        result = HTTPAPI_ERROR;
                    }
                    else
                    {
                        result = HTTPAPI_OK;
                    }
                }
            }
            else
            {
                /*if privatekey comes 1st and certificate is not set yet, then return OK and wait for the certificate to be set*/
                result = HTTPAPI_OK;
            }
            result = HTTPAPI_OK;
        }
        else if (strcmp("x509certificate", optionName) == 0)
        {
            httpHandleData->x509certificate = value;
            if (httpHandleData->x509privatekey != NULL)
            {
                if (curl_easy_setopt(httpHandleData->curl, CURLOPT_SSL_CTX_FUNCTION, ssl_ctx_callback) != CURLE_OK)
                {
                    LogError("unable to curl_easy_setopt");
                    result = HTTPAPI_ERROR;
                }
                else
                {
                    if (curl_easy_setopt(httpHandleData->curl, CURLOPT_SSL_CTX_DATA, httpHandleData) != CURLE_OK)
                    {
                        LogError("unable to curl_easy_setopt");
                        result = HTTPAPI_ERROR;
                    }
                    else
                    {
                        result = HTTPAPI_OK;
                    }
                }
            }
            else
            {
                /*if certificate comes 1st and private key is not set yet, then return OK and wait for the private key to be set*/
                result = HTTPAPI_OK;
            }
            result = HTTPAPI_OK;
        }
        else
        {
            result = HTTPAPI_INVALID_ARG;
            LogError("unknown option %s", optionName);
        }
    }
    return result;
}

HTTPAPI_RESULT HTTPAPI_CloneOption(const char* optionName, const void* value, const void** savedValue)
{
    HTTPAPI_RESULT result;
    if (
        (optionName == NULL) ||
        (value == NULL) ||
        (savedValue == NULL)
        )
    {
        result = HTTPAPI_INVALID_ARG;
        LogError("invalid argument(NULL) passed to HTTPAPI_CloneOption");
    }
    else
    {
        if (strcmp("timeout", optionName) == 0)
        {
            /*by convention value is pointing to an unsigned int */
            unsigned int* temp = malloc(sizeof(unsigned int)); /*shall be freed by HTTPAPIEX*/
            if (temp == NULL)
            {
                result = HTTPAPI_ERROR;
                LogError("malloc failed (result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
            }
            else
            {
                *temp = *(const unsigned int*)value;
                *savedValue = temp;
                result = HTTPAPI_OK;
            }
        }
        else if (strcmp("x509certificate", optionName) == 0)
        {
            /*this is getting the x509 certificate. In this case, value is a pointer to a const char* that contains the certificate as a null terminated string*/
            if (mallocAndStrcpy_s((char**)savedValue, value) != 0)
            {
                LogError("unable to clone the x509 certificate content");
                result = HTTPAPI_ERROR;
            }
            else
            {
                /*return OK when the certificate has been clones successfully*/
                result = HTTPAPI_OK;
            }
        }
        else if (strcmp("x509privatekey", optionName) == 0)
        {
            /*this is getting the x509 private key. In this case, value is a pointer to a const char* that contains the private key as a null terminated string*/
            if (mallocAndStrcpy_s((char**)savedValue, value) != 0)
            {
                LogError("unable to clone the x509 private key content");
                result = HTTPAPI_ERROR;
            }
            else
            {
                /*return OK when the private key has been clones successfully*/
                result = HTTPAPI_OK;
            }
        }
        /*all "long" options are cloned in the same way*/
        else if (
            (strcmp("CURLOPT_LOW_SPEED_LIMIT", optionName) == 0) ||
            (strcmp("CURLOPT_LOW_SPEED_TIME", optionName) == 0) ||
            (strcmp("CURLOPT_FRESH_CONNECT", optionName) == 0) ||
            (strcmp("CURLOPT_FORBID_REUSE", optionName) == 0) ||
            (strcmp("CURLOPT_VERBOSE", optionName) == 0) 
            )
        {
            /*by convention value is pointing to an long */
            long* temp = malloc(sizeof(long)); /*shall be freed by HTTPAPIEX*/
            if (temp == NULL)
            {
                result = HTTPAPI_ERROR;
                LogError("malloc failed (result = %s)", ENUM_TO_STRING(HTTPAPI_RESULT, result));
            }
            else
            {
                *temp = *(const long*)value;
                *savedValue = temp;
                result = HTTPAPI_OK;
            }
        }
        else
        {
            result = HTTPAPI_INVALID_ARG;
            LogError("unknown option %s", optionName);
        }
    }
    return result;
}
