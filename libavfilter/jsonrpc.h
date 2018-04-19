#include <jansson.h>

#define JSONRPC_PARSE_ERROR -32700
#define JSONRPC_INVALID_REQUEST -32600
#define JSONRPC_METHOD_NOT_FOUND -32601
#define JSONRPC_INVALID_PARAMS -32602
#define JSONRPC_INTERNAL_ERROR -32603

typedef int (*jsonrpc_method_prototype)(json_t *json_params, json_t **result, void *userdata);
struct jsonrpc_method_entry_t
{
	const char *name;
	jsonrpc_method_prototype funcptr;
	const char *params_spec;
};
char *jsonrpc_handler(const char *input, size_t input_len, struct jsonrpc_method_entry_t method_table[],
	void *userdata);

json_t *jsonrpc_error_object(int code, const char *message, json_t *data);
json_t *jsonrpc_error_object_predefined(int code, json_t *data);
json_t *jsonrpc_error_response(json_t *json_id, json_t *json_error);
json_t *jsonrpc_result_response(json_t *json_id, json_t *json_result);
json_t *jsonrpc_validate_request(json_t *json_request, const char **str_method, json_t **json_params, json_t **json_id);
json_t *jsonrpc_validate_params(json_t *json_params, const char *params_spec);
json_t *jsonrpc_handle_request_single(json_t *json_request, struct jsonrpc_method_entry_t method_table[],
                                      void *userdata);
char *jsonrpc_parser(const char *input, size_t input_len,
                     void *userdata);
char *jsonrpc_parser_file(const char *file,
                          void *userdata);
