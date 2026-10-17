/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors
  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.
*/

#ifndef cJSON__h
#define cJSON__h

#ifdef __cplusplus
extern "C"
{
#endif

#define cJSON_Invalid (0)
#define cJSON_False  (1 << 0)
#define cJSON_True   (1 << 1)
#define cJSON_NULL   (1 << 2)
#define cJSON_Number (1 << 3)
#define cJSON_String (1 << 4)
#define cJSON_Array  (1 << 5)
#define cJSON_Object (1 << 6)
#define cJSON_Raw    (1 << 7)

#define cJSON_IsReference 256
#define cJSON_StringIsConst 512

typedef struct cJSON
{
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;

typedef struct cJSON_Hooks
{
      void *(*malloc_fn)(size_t sz);
      void (*free_fn)(void *ptr);
} cJSON_Hooks;

void cJSON_InitHooks(cJSON_Hooks* hooks);
cJSON *cJSON_Parse(const char *value);
void   cJSON_Delete(cJSON *item);

int cJSON_GetArraySize(const cJSON *array);
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
cJSON *cJSON_GetObjectItem(const cJSON * const object, const char * const string);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string);
int cJSON_HasObjectItem(const cJSON *object, const char *string);

const char *cJSON_GetErrorPtr(void);

#define cJSON_IsInvalid(item) ((item == NULL) || (item->type == cJSON_Invalid))
#define cJSON_IsFalse(item) ((item != NULL) && ((item->type & 0xFF) == cJSON_False))
#define cJSON_IsTrue(item) ((item != NULL) && ((item->type & 0xFF) == cJSON_True))
#define cJSON_IsBool(item) ((item != NULL) && (((item->type & 0xFF) == cJSON_True) || ((item->type & 0xFF) == cJSON_False)))
#define cJSON_IsNull(item) ((item != NULL) && ((item->type & 0xFF) == cJSON_NULL))
#define cJSON_IsNumber(item) ((item != NULL) && ((item->type & 0xFF) == cJSON_Number))
#define cJSON_IsString(item) ((item != NULL) && ((item->type & 0xFF) == cJSON_String))
#define cJSON_IsArray(item) ((item != NULL) && ((item->type & 0xFF) == cJSON_Array))
#define cJSON_IsObject(item) ((item != NULL) && ((item->type & 0xFF) == cJSON_Object))
#define cJSON_IsRaw(item) ((item != NULL) && ((item->type & 0xFF) == cJSON_Raw))

#ifdef __cplusplus
}
#endif

#endif
