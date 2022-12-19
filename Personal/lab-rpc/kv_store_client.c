/**
 * Client binary.
 */

#include "kv_store_client.h"

#define HOST "localhost"

CLIENT* clnt_connect(char* host) {
  CLIENT* clnt = clnt_create(host, KVSTORE, KVSTORE_V1, "udp");
  if (clnt == NULL) {
    clnt_pcreateerror(host);
    exit(1);
  }
  return clnt;
}

int example(int input) {
  CLIENT *clnt = clnt_connect(HOST);

  int ret;
  int *result;

  result = example_1(&input, clnt);
  if (result == (int *)NULL) {
    clnt_perror(clnt, "call failed");
    exit(1);
  }
  ret = *result;
  xdr_free((xdrproc_t)xdr_int, (char *)result);

  clnt_destroy(clnt);
  
  return ret;
}

char* echo(char* input) {
  CLIENT *clnt = clnt_connect(HOST);

  char* ret;

  /* TODO */
  char** result = echo_1(&input, clnt);
  if (result == (char**) NULL) {
    clnt_perror(clnt, "call failed");
    exit(1);
  }
  ret = *result;
  xdr_free((xdrproc_t)xdr_int, (char *) *result);

  clnt_destroy(clnt);
  
  return ret;
}

void put(buf key, buf value) {
  CLIENT *clnt = clnt_connect(HOST);

  /* TODO */
  // putbuf* putRequest = malloc(sizeof(putRequest));
  // putRequest->putbuf_val = malloc(2 * sizeof(buf));
  // putRequest->putbuf_len = 2;
  // // putRequest->putbuf_val[0] = key;
  // // putRequest->putbuf_val[1] = value;
  // putRequest->putbuf_val[0].buf_len = key.buf_len;
  // putRequest->putbuf_val[0].buf_val = malloc(sizeof(char) * putRequest->putbuf_val[0].buf_len);
  // strcpy(putRequest->putbuf_val[0].buf_val, key.buf_val);

  // putRequest->putbuf_val[1].buf_len = value.buf_len;
  // putRequest->putbuf_val[1].buf_val = malloc(sizeof(char) * putRequest->putbuf_val[1].buf_len);
  // strcpy(putRequest->putbuf_val[1].buf_val, value.buf_val);

  static putbuf putBuffer;

  putBuffer.key = key;
  putBuffer.value = value;
  // putBuffer[0].buf_len = key.buf_len;
  // strcpy(putBuffer[0].buf_val, key.buf_val);
  // putBuffer[1].buf_len = value.buf_len;
  // strcpy(putBuffer[1].buf_val, value.buf_val);

  put_1(&putBuffer, clnt);

  xdr_free((xdrproc_t)xdr_int, (putbuf*) &putBuffer);

  clnt_destroy(clnt);
}

buf* get(buf key) {
  CLIENT *clnt = clnt_connect(HOST);

  buf* ret;

  /* TODO */
  ret = malloc(sizeof(buf));

  buf* getResult = get_1(&key, clnt);
  if (getResult == (buf*) NULL) {
    ret->buf_len = 0;
  } else {
    ret->buf_len = getResult->buf_len;
    ret->buf_val = malloc(sizeof(char) * ret->buf_len);
    memcpy(ret->buf_val, getResult->buf_val, ret->buf_len);
  }
  xdr_free((xdrproc_t)xdr_int, (buf*) getResult);

  clnt_destroy(clnt);
  
  return ret;
}
