/*
 *  Copyright 2016 by SEAL-ORAM Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

//
// Created by Dong Xie on 7/16/2016.
//

#include "RecursiveBinaryORAM.h"

#include <cmath>
#include <cryptopp/osrng.h>
#include "MongoConnector.h"
#include "Util.h"

using namespace CryptoPP;

RecursiveBinaryORAM::RecursiveBinaryORAM(const uint32_t& n) {
    height = (uint32_t)ceil(log2((double)n)) + 1;
    n_buckets = (uint32_t)1 << (height - 1);
    bucket_size = Alpha * (uint32_t)ceil(log2((double)n));

    pos_map = new BinaryORAM(n / pos_base + 1);
    conn = new MongoConnector(server_host, "oram.newrecursivebinary");

    key = new byte[Util::key_length];
    Util::prng.GenerateBlock(key, Util::key_length);
    sbuffer = new std::string[2];

    for (uint32_t i = 0; i < (1 << (height - 1)) - 1; ++i) {
        for (size_t j = 0; j < bucket_size; j ++) {
            std::string tmp  = Util::generate_random_block(B - Util::aes_block_size - sizeof(uint32_t));
            int32_t dummyID = -1;
            std::string dID = std::string((const char *)(& dummyID), sizeof(uint32_t));
            std::string cipher;
            Util::aes_encrypt(dID + tmp, key, cipher);
            conn->insert(i * bucket_size + j, cipher);
        }
    }

    uint32_t cur_id = 0;
    for (uint32_t i = (1 << (height - 1)) - 1; i < (1 << height) - 1; ++i) {
        if (cur_id < n) {
            std::string tmp  = Util::generate_random_block(B - Util::aes_block_size - 3 * sizeof(uint32_t));
            std::string bID = std::string((const char *)(& cur_id), sizeof(uint32_t));
            std::string cipher;
            Util::aes_encrypt(bID + bID + bID + tmp, key, cipher);
            conn->insert(i * bucket_size, cipher);
            checkAndUpdatePosMap(cur_id, cur_id);
            cur_id ++;
        } else {
            std::string tmp  = Util::generate_random_block(B - Util::aes_block_size - sizeof(uint32_t));
            int32_t dummyID = -1;
            std::string dID = std::string((const char *)(& dummyID), sizeof(uint32_t));
            std::string cipher;
            Util::aes_encrypt(dID + tmp, key, cipher);
            conn->insert(i * bucket_size, cipher);
        }
        for (size_t j = 1; j < bucket_size; j ++) {
            std::string tmp  = Util::generate_random_block(B - Util::aes_block_size - sizeof(uint32_t));
            int32_t dummyID = -1;
            std::string dID = std::string((const char *)(& dummyID), sizeof(uint32_t));
            std::string cipher;
            Util::aes_encrypt(dID + tmp, key, cipher);
            conn->insert(i * bucket_size + j, cipher);
        }
    }
}

RecursiveBinaryORAM::~RecursiveBinaryORAM() {
    delete[] key;
    delete[] sbuffer;
    delete pos_map;
    delete conn;
}

std::string RecursiveBinaryORAM::get(const std::string & key) {
    std::string res;
    uint32_t int_key;
    sscanf(key.c_str(), "%d", &int_key);
    access('r', int_key, res);
    return res;
}

void RecursiveBinaryORAM::put(const std::string & key, const std::string & value) {
    uint32_t int_key;
    sscanf(key.c_str(), "%d", &int_key);
    std::string value2 = value;
    access('w', int_key, value2);
}

std::string RecursiveBinaryORAM::get(const uint32_t & key) {
    std::string res;
    access('r', key, res);
    return res;
}

void RecursiveBinaryORAM::put(const uint32_t & key, const std::string & value) {
    std::string value2 = value;
    access('w', key, value2);
}

void RecursiveBinaryORAM::access(const char& op, const uint32_t& block_id, std::string& data) {
    uint32_t new_x = Util::rand_int(n_buckets);
    std::string str_newx = std::string((const char *)(& new_x), sizeof(uint32_t));
    uint32_t x = checkAndUpdatePosMap(block_id, new_x);

    fetchAlongPath(block_id, x);

    if (op == 'r') {
        data = sbuffer[0].substr(sizeof(uint32_t));
    }
    std::string blockID = std::string((const char *)(& block_id), sizeof(uint32_t));
    sbuffer[0] = blockID + data.substr(0, sizeof(uint32_t)) + str_newx + data.substr(2 * sizeof(uint32_t), sizeof(uint32_t));

    bool find = false;
    for (uint32_t i = 0; i < bucket_size; ++ i) {
        readBlock(0, 0, i);

        std::string plain;
        Util::aes_decrypt(sbuffer[1], key, plain);

        if (!find) {
            int32_t b_id;
            memcpy(&b_id, plain.c_str(), sizeof(uint32_t));
            if (b_id != -1) {
                std::string cipher;
                Util::aes_encrypt(plain, key, cipher);

                sbuffer[1] = cipher;
                writeBlock(0, 0, i);
            } else {
                find = true;

                std::string cipher;
                Util::aes_encrypt(sbuffer[0], key, cipher);

                sbuffer[1] = cipher;
                writeBlock(0, 0, i);
            }
        } else {
            std::string cipher;
            Util::aes_encrypt(plain, key, cipher);

            sbuffer[1] = cipher;
            writeBlock(0, 0, i);
        }
    }

    evict();
}

void RecursiveBinaryORAM::fetchAlongPath(const uint32_t& block_id, const uint32_t& x) {
    bool find = false;
    uint32_t cur_pos = x + (1 << (height - 1));
    while (cur_pos > 0) {
        uint32_t id = (cur_pos - 1) * bucket_size;
        for (uint32_t i = 0; i < bucket_size; ++ i) {
            sbuffer[1] = conn->find(id + i);

            std::string plain;
            Util::aes_decrypt(sbuffer[1], key, plain);

            if (!find) {
                int32_t b_id;
                memcpy(&b_id, plain.c_str(), sizeof(uint32_t));

                if (b_id != block_id) {
                    std::string cipher;
                    Util::aes_encrypt(plain, key, cipher);
                    conn->update(id + i, cipher);
                } else {
                    find = true;
                    sbuffer[0] = plain;

                    std::string plain = Util::generate_random_block(B - Util::aes_block_size - sizeof(uint32_t));
                    int32_t dummyID = -1;
                    std::string dID = std::string((const char *)(& dummyID), sizeof(uint32_t));
                    std::string cipher;
                    Util::aes_encrypt(dID + plain, key, cipher);
                    conn->update(id + i, cipher);
                }
            } else {
                std::string cipher;
                Util::aes_encrypt(plain, key, cipher);
                conn->update(id + i, cipher);
            }
        }
        cur_pos >>= 1;
    }
}

void RecursiveBinaryORAM::readBlock(const uint32_t level, const uint32_t bucket, const uint32_t piece) {
    uint32_t bid = ((1 << level) - 1 + bucket) * bucket_size + piece;
    sbuffer[1] = conn->find(bid);
}

void RecursiveBinaryORAM::writeBlock(const uint32_t level, const uint32_t bucket, const uint32_t piece) {
    uint32_t bid = ((1 << level) - 1 + bucket) * bucket_size + piece;
    conn->update(bid, sbuffer[1]);
}

void RecursiveBinaryORAM::evict(const uint32_t level, const uint32_t bucket) {
    bool find = false;
    int32_t blockID = -1;

    //read
    for (uint32_t piece = 0; piece < bucket_size; piece ++) {
        readBlock(level, bucket, piece);

        std::string plain;
        Util::aes_decrypt(sbuffer[1], key, plain);

        if (!find) {
            int32_t b_id;
            memcpy(&b_id, plain.c_str(), sizeof(uint32_t));
            if (b_id == -1) {
                std::string cipher;
                Util::aes_encrypt(plain, key, cipher);
                sbuffer[1] = cipher;
                writeBlock(level, bucket, piece);
            } else {
                find = true;
                blockID = b_id;
                sbuffer[0] = plain;

                std::string plain  = Util::generate_random_block(B - Util::aes_block_size - sizeof(uint32_t));
                int32_t dummyID = -1;
                std::string dID = std::string((const char *)(& dummyID), sizeof(uint32_t));
                std::string cipher;
                Util::aes_encrypt(dID + plain, key, cipher);
                sbuffer[1] = cipher;
                writeBlock(level, bucket, piece);
            }
        } else {
            std::string cipher;
            Util::aes_encrypt(plain, key, cipher);
            sbuffer[1] = cipher;
            writeBlock(level, bucket, piece);
        }
    }

    if (!find) {
        std::string plain  = Util::generate_random_block(B - Util::aes_block_size - sizeof(uint32_t));
        int32_t dummyID = -1;
        std::string dID = std::string((const char *)(& dummyID), sizeof(uint32_t));
        sbuffer[0] = dID + plain;
    }

    //write
    uint32_t bit;
    if (blockID != -1) {
        uint32_t bit1;
        memcpy(&bit1, sbuffer[0].substr(2 * sizeof(uint32_t)).c_str(), sizeof(uint32_t));
        uint32_t bit2 = 1 << (height - level - 2);
        bit = bit1 & bit2;
    } else bit = 1;

    uint32_t lBucket = bucket << 1;
    for (uint32_t nextBucket = 0; nextBucket < 2; nextBucket ++) {
        uint32_t thisBucket = lBucket + nextBucket;

        if (bit != 0 && nextBucket != 0 || bit == 0 && nextBucket == 0) {
            bool find2 = false;
            for (uint32_t i = 0; i < bucket_size; ++ i) {
                readBlock(level + 1, thisBucket, i);

                std::string plain;
                Util::aes_decrypt(sbuffer[1], key, plain);
                if (!find2) {
                    int32_t b_id;
                    memcpy(&b_id, plain.c_str(), sizeof(uint32_t));
                    if (b_id != -1) {
                        std::string cipher;
                        Util::aes_encrypt(plain, key, cipher);
                        sbuffer[1] = cipher;
                        writeBlock(level + 1, thisBucket, i);
                    } else {
                        find2 = true;
                        std::string cipher;
                        Util::aes_encrypt(sbuffer[0], key, cipher);
                        sbuffer[1] = cipher;
                        writeBlock(level + 1, thisBucket, i);
                    }
                } else {
                    std::string cipher;
                    Util::aes_encrypt(plain, key, cipher);
                    sbuffer[1] = cipher;
                    writeBlock(level + 1, thisBucket, i);
                }
            }
        } else {
            for (uint32_t i = 0; i < bucket_size; ++ i) {
                readBlock(level + 1, thisBucket, i);
                std::string plain;
                Util::aes_decrypt(sbuffer[1], key, plain);
                std::string cipher;
                Util::aes_encrypt(plain, key, cipher);
                sbuffer[1] = cipher;
                writeBlock(level + 1, thisBucket, i);
            }
        }
    }
}

void RecursiveBinaryORAM::evict() {
    /* Suppose the eviction rate is not less than 2. */
    //Level 0:
    evict(0, 0);

    //Level 1:
    for (uint32_t bucket = 0; bucket < 2; bucket ++) {
        evict(1, bucket);
    }

    //The following levels:
    for (uint32_t level = 2; level < height - 1; level ++) {
        for (size_t i = 0; i < Gamma; i ++) {
            uint32_t bucket = Util::rand_int(1 << level);
            evict(level, bucket);
        }
    }
}

uint32_t RecursiveBinaryORAM::checkAndUpdatePosMap(const uint32_t& id, const uint32_t& pos) {
    std::string cur = pos_map->get(id / pos_base);
    uint32_t ans;
    memcpy(&ans, cur.c_str() + sizeof(uint32_t) * (id % pos_base), sizeof(uint32_t));
    cur.replace(sizeof(uint32_t) * (id % pos_base), sizeof(uint32_t), (const char*)&pos, sizeof(uint32_t));
    pos_map->put(id / pos_base, cur);
    return ans;
}

uint32_t RecursiveBinaryORAM::getPosMap(const uint32_t& id) {
    std::string cur = pos_map->get(id / pos_base);
    uint32_t ans;
    memcpy(&ans, cur.c_str() + sizeof(uint32_t) * (id % pos_base), sizeof(uint32_t));
    return ans;
}
