/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MPLAYER_M_PROPERTY_H
#define MPLAYER_M_PROPERTY_H

#include <stdbool.h>
#include <stdint.h>

#include "m_option.h"

struct mp_log;

enum mp_property_action {
    // Get the property type. This defines the fundamental data type read from
    // or written to the property.
    // If unimplemented, the m_option entry that defines the property is used.
    //  arg: m_option*
    M_PROPERTY_GET_TYPE,

    // Get the current value.
    //  arg: pointer to a variable of the type according to the property type
    M_PROPERTY_GET,

    // Set a new value. The property wrapper will make sure that only valid
    // values are set (e.g. according to the property type's min/max range).
    // If unimplemented, the property is read-only.
    //  arg: pointer to a variable of the type according to the property type
    M_PROPERTY_SET,

    // Get human readable string representing the current value.
    // If unimplemented, the property wrapper uses the property type as
    // fallback.
    //  arg: char**
    M_PROPERTY_PRINT,

    // Like M_PROPERTY_GET_TYPE, but get a type that is compatible to the real
    // type, but reflect practical limits, such as runtime-available values.
    // This is mostly used for "UI" related things.
    // (Example: volume property.)
    M_PROPERTY_GET_CONSTRICTED_TYPE,

    // Switch the property up/down by a given value.
    // If unimplemented, the property wrapper uses the property type as
    // fallback.
    //  arg: struct m_property_switch_arg*
    M_PROPERTY_SWITCH,

    // Get a string containing a parseable representation.
    // Can't be overridden by property implementations.
    //  arg: char**
    M_PROPERTY_GET_STRING,

    // Set a new value from a string. The property wrapper parses this using the
    // parse function provided by the property type.
    // Can't be overridden by property implementations.
    //  arg: char*
    M_PROPERTY_SET_STRING,

    // Set a mpv_node value.
    //  arg: mpv_node*
    M_PROPERTY_GET_NODE,

    // Get a mpv_node value.
    //  arg: mpv_node*
    M_PROPERTY_SET_NODE,

    // Multiply numeric property with a factor.
    //  arg: double*
    M_PROPERTY_MULTIPLY,

    // Pass down an action to a sub-property.
    //  arg: struct m_property_action_arg*
    M_PROPERTY_KEY_ACTION,
};

// Argument for M_PROPERTY_SWITCH
struct m_property_switch_arg {
    double inc;         // value to add to property, or cycle direction
    bool wrap;          // whether value should wrap around on over/underflow
};

// Argument for M_PROPERTY_KEY_ACTION
struct m_property_action_arg {
    const char* key;
    int action;
    void* arg;
};

enum mp_property_return {
    // Returned on success.
    M_PROPERTY_OK = 1,

    // Returned from validator if action should be executed.
    M_PROPERTY_VALID = 2,

    // Returned on error.
    M_PROPERTY_ERROR = 0,

    // Returned when the property can't be used, for example video related
    // properties while playing audio only.
    M_PROPERTY_UNAVAILABLE = -1,

    // Returned if the requested action is not implemented.
    M_PROPERTY_NOT_IMPLEMENTED = -2,

    // Returned when asking for a property that doesn't exist.
    M_PROPERTY_UNKNOWN = -3,

    // When trying to set invalid or incorrectly formatted data.
    M_PROPERTY_INVALID_FORMAT = -4,
};

struct m_property {
    const char *name;
    // ctx: opaque caller context, which the property might use
    // prop: pointer to this struct
    // action: one of enum mp_property_action
    // arg: specific to the action
    // returns: one of enum mp_property_return
    int (*call)(void *ctx, struct m_property *prop, int action, void *arg);
    void *priv;
    // Special-case: mark options for which command.c uses the option-bridge
    bool is_option;
};

struct m_property *m_property_list_find(const struct m_property *list,
                                        const char *name);

// Access a property.
// action: one of m_property_action
// ctx: opaque value passed through to property implementation
// returns: one of mp_property_return
int m_property_do(struct mp_log *log, const struct m_property* prop_list,
                  const char* property_name, int action, void* arg, void *ctx);

// Given a path of the form "a/b/c", this function will set *prefix to "a",
// and rem to "b/c", and return true.
// If there is no '/' in the path, set prefix to path, and rem to "", and
// return false.
bool m_property_split_path(const char *path, bstr *prefix, char **rem);

// Print a list of properties.
void m_properties_print_help_list(struct mp_log *log,
                                  const struct m_property *list);

// Expand a property string.
// This function allows to print strings containing property values.
//  ${NAME} is expanded to the value of property NAME.
//  If NAME starts with '=', use the raw value of the property.
//  ${NAME:STR} expands to the property, or STR if the property is not
//  available.
//  ${?NAME:STR} expands to STR if the property is available.
//  ${!NAME:STR} expands to STR if the property is not available.
// General syntax: "${" ["?" | "!"] ["="] NAME ":" STR "}"
// STR is recursively expanded using the same rules.
// "$$" can be used to escape "$", and "$}" to escape "}".
// "$>" disables parsing of "$" for the rest of the string.
char* m_properties_expand_string(const struct m_property *prop_list,
                                 const char *str, void *ctx);

// Trivial helpers for implementing properties.

inline int m_property_flag_ro_validate(int action, void* arg) {
    switch (action) {
    case M_PROPERTY_GET:
        return M_PROPERTY_VALID;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_FLAG};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

inline int m_property_flag_ro(int action, void* arg, int var)
{
    int ret = m_property_flag_ro_validate(action, arg);
    if (ret != M_PROPERTY_VALID)
        return ret;

    *(int *)arg = !!var;
    return M_PROPERTY_OK;
}

inline int m_property_int_ro(int action, void *arg, int var)
{
    switch (action) {
    case M_PROPERTY_GET:
        *(int *)arg = var;
        return M_PROPERTY_OK;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_INT};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

inline int m_property_int64_ro(int action, void* arg, int64_t var)
{
    switch (action) {
    case M_PROPERTY_GET:
        *(int64_t *)arg = var;
        return M_PROPERTY_OK;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_INT64};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

inline int m_property_float_ro(int action, void *arg, float var)
{
    switch (action) {
    case M_PROPERTY_GET:
        *(float *)arg = var;
        return M_PROPERTY_OK;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_FLOAT};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

inline int m_property_double_ro_validate(int action, void* arg) {
    switch (action) {
    case M_PROPERTY_GET:
        return M_PROPERTY_VALID;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_DOUBLE};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

inline int m_property_double_ro(int action, void *arg, double var)
{
    int ret = m_property_double_ro_validate(action, arg);
    if (ret != M_PROPERTY_VALID) {
        return ret;
    }
    *(double *)arg = var;
    return M_PROPERTY_OK;
}

inline int m_property_strdup_ro(int action, void* arg, const char *var)
{
    if (!var)
        return M_PROPERTY_UNAVAILABLE;
    switch (action) {
    case M_PROPERTY_GET:
        *(char **)arg = talloc_strdup(NULL, var);
        return M_PROPERTY_OK;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_STRING};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}


// If *action is M_PROPERTY_KEY_ACTION, but the associated path is "", then
// make this into a top-level action.
inline static void m_property_unkey(int *action, void **arg)
{
    if (*action == M_PROPERTY_KEY_ACTION) {
        struct m_property_action_arg *ka = *arg;
        if (!ka->key[0]) {
            *action = ka->action;
            *arg = ka->arg;
        }
    }
}

inline int m_property_read_sub_validate(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    m_property_unkey(&action, &arg);
    switch (action) {
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_NODE};
        return M_PROPERTY_OK;
    case M_PROPERTY_GET:
    case M_PROPERTY_PRINT:
    case M_PROPERTY_KEY_ACTION:
        return M_PROPERTY_VALID;
    default:
        return M_PROPERTY_NOT_IMPLEMENTED;
    };
}

struct m_sub_property {
    // Name of the sub-property - this will be prefixed with the parent
    // property's name.
    const char *name;
    // Type of the data stored in the value member. See m_option.
    struct m_option type;
    // Data returned by the sub-property. m_property_read_sub() will make a
    // copy of this if needed. It will never write or free the data.
    union m_option_value value;
    // This can be set to true if the property should be hidden.
    bool unavailable;
};

// Convenience macros which can be used as part of a sub_property entry.
#define SUB_PROP_INT(i) \
    .type = {.type = CONF_TYPE_INT}, .value = {.int_ = (i)}
#define SUB_PROP_INT64(i) \
    .type = {.type = CONF_TYPE_INT64}, .value = {.int64 = (i)}
#define SUB_PROP_STR(s) \
    .type = {.type = CONF_TYPE_STRING}, .value = {.string = (char *)(s)}
#define SUB_PROP_FLOAT(f) \
    .type = {.type = CONF_TYPE_FLOAT}, .value = {.float_ = (f)}
#define SUB_PROP_DOUBLE(f) \
    .type = {.type = CONF_TYPE_DOUBLE}, .value = {.double_ = (f)}
#define SUB_PROP_FLAG(f) \
    .type = {.type = CONF_TYPE_FLAG}, .value = {.flag = (f)}
#define SUB_PROP_PTS(f) \
    .type = {.type = &m_option_type_time}, .value = {.double_ = (f)}

int m_property_read_sub(const struct m_sub_property *props, int action, void *arg);

                                 
// Used with m_property_read_list().
// Get an entry. item is the 0-based index of the item. This behaves like a
// top-level property request (but you must implement M_PROPERTY_GET_TYPE).
// item will be in range [0, count), for count see m_property_read_list()
// action, arg are for property access.
// ctx is userdata passed to m_property_read_list.
typedef int (*m_get_item_cb)(int item, int action, void *arg, void *ctx);

int m_property_read_list(int action, void *arg, int count,
                         m_get_item_cb get_item, void *ctx);

#endif /* MPLAYER_M_PROPERTY_H */
