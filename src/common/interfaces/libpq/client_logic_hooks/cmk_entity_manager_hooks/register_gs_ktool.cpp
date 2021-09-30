/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 *
 * register_gs_ktool.cpp
 *      gs_ktool is an independent key management tool provided by GaussDB Kernel, can generate and store symmetric
 *      key with [16, 112] bytes.
 *      when CREATE CMKO, if KEY_STROE = gs_ktool, then:
 *          1. KEY_PATH: gs_ktool use $key_id to identify keys, KEY_PATH = "gs_ktool/$key_id"
 *          2. ALGORITHM: gs_ktool cannot generate asymmetric key pairs, so keys generated by gs_ktool are only
 *          avaiable for AES_256 algorithm
 *      if you register gs_ktool, you should be sure your system has installed gs_ktool, and the environment variables
 *      and the configuration files are avaiable.
 * 
 * IDENTIFICATION
 *	  src/common/interfaces/libpq/client_logic_hooks/cmk_entity_manager_hooks/register_gs_ktool.cpp
 *
 * -------------------------------------------------------------------------
 */

#include "cmkem_version_control.h"
#ifdef ENABLE_GS_KTOOL

#include "register_gs_ktool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gs_ktool/kt_interface.h"
#include "reg_hook_frame.h"
#include "cmkem_comm_algorithm.h"

const int MAX_KEYPATH_LEN = 64;
const int UPPER_TO_LOWER_OFFSET = 'a' - 'A';

static CmkCacheList *cmk_cache_list = NULL;
static const char *supported_algorithms[] = {"AES_256_CBC", "SM4", NULL};

static CmkemErrCode check_cmk_id_validity(CmkIdentity *cmk_identity);
static CmkemErrCode check_cmk_algo_validity(const char *cmk_algo);
static CmkemErrCode check_cmk_entity_validity(CmkIdentity *cmk_identity);
static void get_cmk_id_from_key_path(const char *key_path, unsigned int *cmk_id);
static CmkemErrCode read_cmk_plain(unsigned int cmk_id, CmkemUStr **cmk_plain);

static ProcessPolicy create_cmk_obj_hookfunc(CmkIdentity *cmk_identity);
static ProcessPolicy encrypt_cek_plain_hookfunc(CmkemUStr *cek_plain, CmkIdentity *cmk_identity,
    CmkemUStr **cek_cipher);
static ProcessPolicy decrypt_cek_cipher_hookfunc(CmkemUStr *cek_cipher, CmkIdentity *cmk_identity,
    CmkemUStr **cek_plain);

static void cmkem_tolower(const char *in, char *out, size_t out_buf_len)
{
    if (out_buf_len < strlen(in) + 1) {
        return;
    }

    size_t i = 0;
    for (; i < strlen(in); i++) {
        if (in[i] >= 'A' && in[i] <= 'Z') {
            out[i] = in[i] + UPPER_TO_LOWER_OFFSET;
        } else {
            out[i] = in[i];
        }
    }
    out[i] = '\0';
}

static CmkemErrCode check_cmk_id_validity(CmkIdentity *cmk_identity)
{
    const char *key_path_tag = "gs_ktool/";
    char tmp_str[MAX_KEYPATH_LEN] = {0};
    int tmp_pos = 0;
    bool has_invalid_char = false;
    const char *cmk_id_str = cmk_identity->cmk_id_str;

    if (strlen(cmk_id_str) <= strlen(key_path_tag)) {
        cmkem_errmsg("invalid key path: '%s', it should be like \"%s1\".", cmk_identity->cmk_id_str, key_path_tag);
        return CMKEM_CHECK_CMK_ID_ERR;
    }

    char cmk_id_lower[strlen(cmk_id_str) + 1] = {0};
    cmkem_tolower(cmk_id_str, cmk_id_lower, sizeof(cmk_id_lower));
    for (size_t i = 0; i < strlen(key_path_tag); i++) {
        if (cmk_id_lower[i] != key_path_tag[i]) {
            cmkem_errmsg("invalid key path: '%s', it should be like \"%s1\".", cmk_id_lower, key_path_tag);
            return CMKEM_CHECK_CMK_ID_ERR;
        }
    }

    for (size_t i = strlen(key_path_tag); i < strlen(cmk_id_str); i++) {
        if (cmk_id_str[i] < '0' || cmk_id_str[i] > '9') {
            has_invalid_char = true;
        }
        tmp_str[tmp_pos] = cmk_id_str[i];
        tmp_pos++;
    }

    if (has_invalid_char) {
        cmkem_errmsg("invalid key path: '%s', '%s' is expected to be an integer.", cmk_identity->cmk_id_str, tmp_str);
        return CMKEM_CHECK_CMK_ID_ERR;
    }

    tmp_str[tmp_pos] = '\0';
    cmk_identity->cmk_id_num = atoi(tmp_str);

    return CMKEM_SUCCEED;
}

static CmkemErrCode check_cmk_algo_validity(const char *cmk_algo)
{
    char error_msg_buf[MAX_CMKEM_ERRMSG_BUF_SIZE] = {0};
    error_t rc = 0;
    
    for (size_t i = 0; supported_algorithms[i] != NULL; i++) {
        if (strcasecmp(cmk_algo, supported_algorithms[i]) == 0) {
            return CMKEM_SUCCEED;
        }
    }

    rc = sprintf_s(error_msg_buf, MAX_CMKEM_ERRMSG_BUF_SIZE, "unpported algorithm '%s', gs_ktool only support: ",
        cmk_algo);
    securec_check_ss_c(rc, "", "");

    for (size_t i = 0; supported_algorithms[i] != NULL; i++) {
        rc = strcat_s(error_msg_buf, MAX_CMKEM_ERRMSG_BUF_SIZE, supported_algorithms[i]);
        securec_check_c(rc, "\0", "\0");
        rc = strcat_s(error_msg_buf, MAX_CMKEM_ERRMSG_BUF_SIZE, "  ");
        securec_check_c(rc, "\0", "\0");
    }

    cmkem_errmsg(error_msg_buf);
    return CMKEM_CHECK_ALGO_ERR;
}

static CmkemErrCode check_cmk_entity_validity(CmkIdentity *cmk_identity)
{
    unsigned int cmk_len = 0;
    
    if (!get_cmk_len(cmk_identity->cmk_id_num, &cmk_len)) {
        cmkem_errmsg("failed to read cmk from gs_ktool, key id: %d.", cmk_identity->cmk_id_num);
        return CMKEM_GS_KTOOL_ERR;
    }

    if (cmk_len != get_key_len_by_algo(get_algo_by_str(cmk_identity->cmk_algo))) {
        return CMKEM_CHECK_ALGO_ERR;
    }

    return CMKEM_SUCCEED;
}

static void get_cmk_id_from_key_path(const char *key_path, unsigned int *cmk_id)
{
    const char *key_path_tag = "gs_ktool/";
    *cmk_id = (unsigned int) atoi(key_path + strlen(key_path_tag));
}

static CmkemErrCode read_cmk_plain(unsigned int cmk_id, CmkemUStr **cmk_plain)
{
    unsigned int tmp_cmk_len = 0;

    *cmk_plain = malloc_cmkem_ustr(AES256_KEY_BUF_LEN);
    if (*cmk_plain == NULL) {
        return CMKEM_MALLOC_MEM_ERR;
    }

    /* case a : try to get cmk plain from cache */
    if (!get_cmk_from_cache(cmk_cache_list, cmk_id, (*cmk_plain)->ustr_val)) {
        /* case b : failed to get cmk plian from cache, try to get it from gs_ktool */
        if (!get_cmk_plain(cmk_id, (*cmk_plain)->ustr_val, &tmp_cmk_len)) {
            free_cmkem_ustr_with_erase(*cmk_plain);
            return CMKEM_GS_KTOOL_ERR;
        }

        push_cmk_to_cache(cmk_cache_list, cmk_id, (*cmk_plain)->ustr_val);
    }

    (*cmk_plain)->ustr_len = (size_t) tmp_cmk_len;
    return CMKEM_SUCCEED;
}

/* LRU cache */
CmkCacheList *init_cmk_cache_list()
{
    CmkCacheList *cmk_cache_list = NULL;
    cmk_cache_list = (CmkCacheList *)malloc(sizeof(CmkCacheList));
    if (cmk_cache_list == NULL) {
        cmkem_errmsg("failed to malloc memory.");
        return NULL;
    }

    cmk_cache_list->cmk_node_cnt = 0;
    cmk_cache_list->first_cmk_node = NULL;
    return cmk_cache_list;
}

void push_cmk_to_cache(CmkCacheList *cmk_cache_list, unsigned int cmk_id, const unsigned char *cmk_plian)
{
    CmkCacheNode *new_node = NULL;
    CmkCacheNode *last_node = NULL;

    new_node = (CmkCacheNode *)malloc(sizeof(CmkCacheNode));
    if (new_node == NULL) {
        cmkem_errmsg("failed to malloc memory.");
        return;
    }
    new_node->cmk_id = cmk_id;
    for (size_t i = 0; i < DEFAULT_CMK_CACHE_LEN; i++) {
        new_node->cmk_plain[i] = cmk_plian[i];
    }
    
    if (cmk_cache_list->cmk_node_cnt < MAX_CMK_CACHE_NODE_CNT) {
        new_node->next = cmk_cache_list->first_cmk_node;
        cmk_cache_list->first_cmk_node = new_node;
        cmk_cache_list->cmk_node_cnt++;
    } else {
        last_node = cmk_cache_list->first_cmk_node;
        while (last_node->next->next != NULL) {
            last_node = last_node->next;
        }
        cmkem_free(last_node->next);
        last_node->next = NULL;
    }
}

bool get_cmk_from_cache(CmkCacheList *cmk_cache_list, unsigned int cmk_id, unsigned char *cmk_plain)
{
    CmkCacheNode *cur_cmk_node = NULL;
    CmkCacheNode *correct_cmk_node = NULL;

    if (cmk_cache_list->first_cmk_node == NULL) {
        return false;
    }

    /* the head node is not used */
    cur_cmk_node = cmk_cache_list->first_cmk_node;
    /* a. there are only 1 node */
    if (cur_cmk_node->next == NULL) {
        if (cur_cmk_node->cmk_id == cmk_id) {
            for (size_t i = 0; i < DEFAULT_CMK_CACHE_LEN; i++) {
                cmk_plain[i] = cur_cmk_node->cmk_plain[i];
            }
            return true;
        }
    } else { /* case b : there are 2 or more nodes */
        /* 
         * if the first node is the correct node, like this : 
         * to find node '2', and the cache list is : '2' -> '1' -> '3'
         */
        if (cur_cmk_node->cmk_id == cmk_id) { 
            for (size_t i = 0; i < DEFAULT_CMK_CACHE_LEN; i++) {
                cmk_plain[i] = cur_cmk_node->cmk_plain[i];
            }
            return true;
        } else {
            while (cur_cmk_node->next != NULL) {
                if (cur_cmk_node->next->cmk_id == cmk_id) {
                    correct_cmk_node = cur_cmk_node->next;
                    for (size_t i = 0; i < DEFAULT_CMK_CACHE_LEN; i++) {
                        cmk_plain[i] = correct_cmk_node->cmk_plain[i];
                    }

                    /* refresh cache list */
                    cur_cmk_node->next = correct_cmk_node->next;
                    correct_cmk_node->next = cmk_cache_list->first_cmk_node;
                    cmk_cache_list->first_cmk_node = correct_cmk_node;
                    return true;
                }
                
                cur_cmk_node = cur_cmk_node->next;
            }
        }
    }

    return false;
}

void free_cmk_cache_list(CmkCacheList *cmk_cahce_list)
{
    CmkCacheNode *to_free = NULL;
    CmkCacheNode *cur_node = NULL;

    if (cmk_cahce_list == NULL) {
        return;
    }

    cur_node = cmk_cahce_list->first_cmk_node;
    while (cur_node != NULL) {
        to_free = cur_node;
        cur_node = cur_node->next;
        cmkem_free(to_free);
    }

    cmkem_free(cmk_cahce_list);
}

static ProcessPolicy create_cmk_obj_hookfunc(CmkIdentity *cmk_identity)
{
    CmkemErrCode ret = CMKEM_SUCCEED;

    if (cmk_identity->cmk_store == NULL || strcasecmp(cmk_identity->cmk_store, "gs_ktool") != 0) {
        return POLICY_CONTINUE;
    }

    if (cmk_identity->cmk_id_str == NULL) {
        cmkem_errmsg("failed to create client master key, failed to find arg: KEY_PATH.");
        return POLICY_ERROR;
    }

    if (cmk_identity->cmk_algo == NULL) {
        cmkem_errmsg("failed to create client master key, failed to find arg: ALGORITHM.");
        return POLICY_ERROR;
    }

    ret = check_cmk_algo_validity(cmk_identity->cmk_algo);
    if (ret != CMKEM_SUCCEED) {
        return POLICY_ERROR;
    }

    ret = check_cmk_id_validity(cmk_identity);
    if (ret != CMKEM_SUCCEED) {
        return POLICY_ERROR;
    }

    ret = check_cmk_entity_validity(cmk_identity);
    if (ret != CMKEM_SUCCEED) {
        return POLICY_ERROR;
    }

    return POLICY_BREAK;
}

static ProcessPolicy encrypt_cek_plain_hookfunc(CmkemUStr *cek_plain, CmkIdentity *cmk_identity, CmkemUStr **cek_cipher)
{
    CmkemErrCode ret = CMKEM_SUCCEED;
    CmkemUStr *cmk_plain = NULL;
    unsigned int cmk_id = 0;
    AlgoType cmk_algo = get_algo_by_str(cmk_identity->cmk_algo);

    if (cmk_identity->cmk_store == NULL || strcasecmp(cmk_identity->cmk_store, "gs_ktool") != 0) {
        return POLICY_CONTINUE;
    }

    get_cmk_id_from_key_path(cmk_identity->cmk_id_str, &cmk_id);

    ret = read_cmk_plain(cmk_id, &cmk_plain);
    if (ret != CMKEM_SUCCEED) {
        return POLICY_ERROR;
    }

    ret = encrypt_with_symm_algo(cmk_algo, cek_plain, cmk_plain, cek_cipher);
    free_cmkem_ustr_with_erase(cmk_plain);
    if (ret != CMKEM_SUCCEED) { 
        return POLICY_ERROR;
    }

    return POLICY_BREAK;
}

static ProcessPolicy decrypt_cek_cipher_hookfunc(CmkemUStr *cek_cipher, CmkIdentity *cmk_identity,
    CmkemUStr **cek_plain)
{
    CmkemErrCode ret = CMKEM_SUCCEED;
    CmkemUStr *cmk_plain = NULL;
    unsigned int cmk_id = 0;
    AlgoType cmk_algo = get_algo_by_str(cmk_identity->cmk_algo);

    if (cmk_identity->cmk_store == NULL || strcasecmp(cmk_identity->cmk_store, "gs_ktool") != 0) {
        return POLICY_CONTINUE;
    }

    get_cmk_id_from_key_path(cmk_identity->cmk_id_str, &cmk_id);

    ret = read_cmk_plain(cmk_id, &cmk_plain);
    if (ret != CMKEM_SUCCEED) {
        return POLICY_ERROR;
    }

    ret = decrypt_with_symm_algo(cmk_algo, cek_cipher, cmk_plain, cek_plain);
    free_cmkem_ustr_with_erase(cmk_plain);
    if (ret != CMKEM_SUCCEED) { 
        return POLICY_ERROR;
    }
    
    return POLICY_BREAK;
}

int reg_cmke_manager_gs_ktool_main()
{
    /* init LRU cache list */
    cmk_cache_list = init_cmk_cache_list();
    if (cmk_cache_list == NULL) {
        return -1;
    }
    
    CmkEntityManager gs_ktool = {
        create_cmk_obj_hookfunc,
        encrypt_cek_plain_hookfunc,
        decrypt_cek_cipher_hookfunc,
        NULL, /* drop_cmk_obj_hook_func: no need */
        NULL, /* post_create_cmk_obj_hook_func: no need */
    };

    return (reg_cmk_entity_manager(gs_ktool) == CMKEM_SUCCEED) ? 0 : -1;
}

#endif /* ENABLE_GS_KTOOL */