/* Stock libmico-common JSON wrappers, matching its disassembled ABI. */
extern void *json_object_new_int(int);
extern void *json_object_new_int64(long long);
extern void json_object_object_add(void *,const char *,void *);
int json_add_int(void *o,const char *key,int v) {
    void *value=json_object_new_int(v);if(!value)return -1;
    json_object_object_add(o,key,value);return 0;
}
int json_add_int64(void *o,const char *key,long long v) {
    void *value=json_object_new_int64(v);if(!value)return -1;
    json_object_object_add(o,key,value);return 0;
}
