// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/we_json.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
} json_lexer_t;

static locale_t g_we_c_locale = (locale_t)0;
static pthread_once_t g_we_c_locale_once = PTHREAD_ONCE_INIT;

static void init_we_c_locale(void)
{
    g_we_c_locale = newlocale(LC_ALL_MASK, "C", (locale_t)0);
}

static inline locale_t we_c_locale(void)
{
    pthread_once(&g_we_c_locale_once, init_we_c_locale);
    return g_we_c_locale;
}

static float we_strtof_c(const char *s, char **endptr)
{
    locale_t cloc = we_c_locale();
    if (cloc != (locale_t)0) {
        return strtof_l(s, endptr, cloc);
    }
    return strtof(s, endptr);
}

static double we_strtod_c(const char *s, char **endptr)
{
    locale_t cloc = we_c_locale();
    if (cloc != (locale_t)0) {
        return strtod_l(s, endptr, cloc);
    }
    return strtod(s, endptr);
}

static const uint64_t RYU_DOUBLE_POW5_INV_SPLIT[292][2] = {
    {0x0000000000000001ULL, 0x2000000000000000ULL},
    {0x999999999999999aULL, 0x1999999999999999ULL},
    {0x47ae147ae147ae15ULL, 0x147ae147ae147ae1ULL},
    {0x6c8b4395810624deULL, 0x10624dd2f1a9fbe7ULL},
    {0x7a786c226809d496ULL, 0x1a36e2eb1c432ca5ULL},
    {0x61f9f01b866e43abULL, 0x14f8b588e368f084ULL},
    {0xb4c7f34938583622ULL, 0x10c6f7a0b5ed8d36ULL},
    {0x87a6520ec08d236aULL, 0x1ad7f29abcaf4857ULL},
    {0x9fb841a566d74f88ULL, 0x15798ee2308c39dfULL},
    {0xe62d01511f12a607ULL, 0x112e0be826d694b2ULL},
    {0xd6ae6881cb5109a4ULL, 0x1b7cdfd9d7bdbab7ULL},
    {0xdef1ed34a2a73aeaULL, 0x15fd7fe17964955fULL},
    {0x7f27f0f6e885c8bbULL, 0x119799812dea1119ULL},
    {0x650cb4be40d60df8ULL, 0x1c25c268497681c2ULL},
    {0xea70909833de7193ULL, 0x16849b86a12b9b01ULL},
    {0x21f3a6e0297ec143ULL, 0x1203af9ee756159bULL},
    {0x6985d7cd0f313537ULL, 0x1cd2b297d889bc2bULL},
    {0x2137dfd73f5a90f9ULL, 0x170ef54646d49689ULL},
    {0xe75fe645cc4873faULL, 0x12725dd1d243aba0ULL},
    {0xa5663d3c7a0d865dULL, 0x1d83c94fb6d2ac34ULL},
    {0x511e976394d79eb1ULL, 0x179ca10c9242235dULL},
    {0xda7edf82dd794bc1ULL, 0x12e3b40a0e9b4f7dULL},
    {0x2a6498d1625bac68ULL, 0x1e392010175ee596ULL},
    {0xeeb6e0a781e2f053ULL, 0x182db34012b25144ULL},
    {0x58924d52ce4f26a9ULL, 0x1357c299a88ea76aULL},
    {0x27507bb7b07ea441ULL, 0x1ef2d0f5da7dd8aaULL},
    {0x52a6c95fc0655034ULL, 0x18c240c4aecb13bbULL},
    {0x0eebd44c99eaa690ULL, 0x13ce9a36f23c0fc9ULL},
    {0xb17953adc3110a80ULL, 0x1fb0f6be50601941ULL},
    {0xc12ddc8b02740867ULL, 0x195a5efea6b34767ULL},
    {0x3424b06f3529a052ULL, 0x14484bfeebc29f86ULL},
    {0x901d59f290ee19dbULL, 0x1039d66589687f9eULL},
    {0x4cfbc31db4b0295fULL, 0x19f623d5a8a73297ULL},
    {0x3d9635b15d59bab2ULL, 0x14c4e977ba1f5bacULL},
    {0x97ab5e277de16228ULL, 0x109d8792fb4c4956ULL},
    {0xf2abc9d8c9689d0dULL, 0x1a95a5b7f87a0ef0ULL},
    {0x5bbca17a3aba173eULL, 0x154484932d2e725aULL},
    {0xafca1ac82efb45cbULL, 0x11039d428a8b8eaeULL},
    {0xb2dcf7a6b1920945ULL, 0x1b38fb9daa78e44aULL},
    {0xf57d92ebc141a104ULL, 0x15c72fb1552d836eULL},
    {0xc46475896767b403ULL, 0x116c262777579c58ULL},
    {0x6d6d88dbd8a5ecd2ULL, 0x1be03d0bf225c6f4ULL},
    {0x8abe071646eb23dbULL, 0x164cfda3281e38c3ULL},
    {0x6efe6c11d255b649ULL, 0x11d7314f534b609cULL},
    {0xb197134fb6ef8a0eULL, 0x1c8b821885456760ULL},
    {0x27ac0f72f8bfa1a5ULL, 0x16d601ad376ab91aULL},
    {0xb95672c260994e1eULL, 0x1244ce242c5560e1ULL},
    {0xf5571e03cdc21695ULL, 0x1d3ae36d13bbce35ULL},
    {0x2aac18030b01ababULL, 0x17624f8a762fd82bULL},
    {0xbbbce0026f348956ULL, 0x12b50c6ec4f31355ULL},
    {0x92c7ccd0b1eda889ULL, 0x1dee7a4ad4b81eefULL},
    {0xdbd30a408e57ba07ULL, 0x17f1fb6f10934bf2ULL},
    {0x7ca8d50071dfc806ULL, 0x1327fc58da0f6ff5ULL},
    {0xfaa7bb33e9660cd6ULL, 0x1ea6608e29b24cbbULL},
    {0x9552fc298784d711ULL, 0x18851a0b548ea3c9ULL},
    {0xaaa8c9bad2d0ac0eULL, 0x139dae6f76d88307ULL},
    {0xdddadc5e1e1aace3ULL, 0x1f62b0b257c0d1a5ULL},
    {0x7e48b04b4b488a4fULL, 0x191bc08eac9a4151ULL},
    {0xcb6d59d5d5d3a1d9ULL, 0x141633a556e1cddaULL},
    {0x3c577b1177dc817bULL, 0x1011c2eaabe7d7e2ULL},
    {0xc6f25e825960cf2aULL, 0x19b604aaaca62636ULL},
    {0x6bf518684780a5bbULL, 0x14919d5556eb51c5ULL},
    {0x232a79ed06008496ULL, 0x10747ddddf22a7d1ULL},
    {0xd1dd8fe1a3340756ULL, 0x1a53fc9631d10c81ULL},
    {0xa7e4731ae8f66c45ULL, 0x150ffd44f4a73d34ULL},
    {0x531d28e253f8569eULL, 0x10d9976a5d52975dULL},
    {0xeb61db03b98d5762ULL, 0x1af5bf109550f22eULL},
    {0xbc4e48cfc7a445e8ULL, 0x159165a6ddda5b58ULL},
    {0x6371d3d96c836b20ULL, 0x11411e1f17e1e2adULL},
    {0x9f1c8628ad9f11cdULL, 0x1b9b6364f3030448ULL},
    {0xe5b06b53be18db0bULL, 0x1615e91d8f359d06ULL},
    {0xeaf3890fcb4715a2ULL, 0x11ab20e472914a6bULL},
    {0x44b8db4c7871bc37ULL, 0x1c45016d841baa46ULL},
    {0x03c715d6c6c1635fULL, 0x169d9abe03495505ULL},
    {0x3638de456bcde919ULL, 0x1217aefe69077737ULL},
    {0x56c163a2461641c1ULL, 0x1cf2b1970e725858ULL},
    {0xdf011c81d1ab67ceULL, 0x17288e1271f51379ULL},
    {0x7f3416ce4155eca5ULL, 0x1286d80ec190dc61ULL},
    {0x6520247d3556476eULL, 0x1da48ce468e7c702ULL},
    {0xea801d30f7783925ULL, 0x17b6d71d20b96c01ULL},
    {0xbb99b0f3f92cfa84ULL, 0x12f8ac174d612334ULL},
    {0x5f5c4e532847f739ULL, 0x1e5aacf215683854ULL},
    {0x7f7d0b75b9d32c2eULL, 0x18488a5b44536043ULL},
    {0x9930d5f7c7dc2358ULL, 0x136d3b7c36a919cfULL},
    {0x8eb4898c72f9d226ULL, 0x1f152bf9f10e8fb2ULL},
    {0x722a07a38f2e41b8ULL, 0x18ddbcc7f40ba628ULL},
    {0xc1bb394fa5be9afaULL, 0x13e497065cd61e86ULL},
    {0x9c5ec2190930f7f6ULL, 0x1fd424d6faf030d7ULL},
    {0x49e56814075a5ff8ULL, 0x197683df2f268d79ULL},
    {0x6e51201005e1e660ULL, 0x145ecfe5bf520ac7ULL},
    {0xf1da800cd181851aULL, 0x104bd984990e6f05ULL},
    {0x4fc400148268d4f5ULL, 0x1a12f5a0f4e3e4d6ULL},
    {0xd96999aa01ed772bULL, 0x14dbf7b3f71cb711ULL},
    {0xadee1488018ac5bcULL, 0x10aff95cc5b09274ULL},
    {0x497ceda668de092cULL, 0x1ab328946f80ea54ULL},
    {0x3aca57b853e4d424ULL, 0x155c2076bf9a5510ULL},
    {0x623b7960431d7683ULL, 0x1116805effaeaa73ULL},
    {0x9d2bf566d1c8bd9eULL, 0x1b5733cb32b110b8ULL},
    {0x7dbcc452416d647fULL, 0x15df5ca28ef40d60ULL},
    {0xcafd69db678ab6ccULL, 0x117f7d4ed8c33de6ULL},
    {0xab2f0fc572778adfULL, 0x1bff2ee48e052fd7ULL},
    {0x88f273045b92d580ULL, 0x1665bf1d3e6a8cacULL},
    {0xd3f528d049424466ULL, 0x11eaff4a98553d56ULL},
    {0xb988414d4203a0a3ULL, 0x1cab3210f3bb9557ULL},
    {0x6139cdd76802e6e9ULL, 0x16ef5b40c2fc7779ULL},
    {0xe761717920025254ULL, 0x125915cd68c9f92dULL},
    {0xa568b58e999d5086ULL, 0x1d5b561574765b7cULL},
    {0x5120913ee14aa6d2ULL, 0x177c44ddf6c515fdULL},
    {0xa74d40ff1aa21f0eULL, 0x12c9d0b1923744caULL},
    {0x0baece64f769cb4aULL, 0x1e0fb44f50586e11ULL},
    {0x3c8bd850c5ee3c3bULL, 0x180c903f7379f1a7ULL},
    {0xca0979da37f1c9c9ULL, 0x133d4032c2c7f485ULL},
    {0xa9a8c2f6bfe942dbULL, 0x1ec866b79e0cba6fULL},
    {0x2153cf2bccba9be3ULL, 0x18a0522c7e709526ULL},
    {0x1aa9728970954982ULL, 0x13b374f06526ddb8ULL},
    {0xf775840f1a88759dULL, 0x1f8587e7083e2f8cULL},
    {0x5f9136727ba05e17ULL, 0x19379fec0698260aULL},
    {0x1940f85b9619e4dfULL, 0x142c7ff0054684d5ULL},
    {0xe100c6afab47ea4cULL, 0x1023998cd1053710ULL},
    {0xce67a44c453fdd47ULL, 0x19d28f47b4d524e7ULL},
    {0xd852e9d69dccb106ULL, 0x14a8729fc3ddb71fULL},
    {0x79dbee454b0a2738ULL, 0x1086c219697e2c19ULL},
    {0x295fe3a211a9d859ULL, 0x1a71368f0f30468fULL},
    {0xbab31c81a7bb137aULL, 0x15275ed8d8f36ba5ULL},
    {0x6228e39aec95a92fULL, 0x10ec4be0ad8f8951ULL},
    {0x9d0e38f7e0ef7517ULL, 0x1b13ac9aaf4c0ee8ULL},
    {0xb0d82d931a592a79ULL, 0x15a956e225d67253ULL},
    {0x8d79be0f4847552eULL, 0x11544581b7dec1dcULL},
    {0x158f967eda0bbb7cULL, 0x1bba08cf8c979c94ULL},
    {0x77a611ff14d62f97ULL, 0x162e6d72d6dfb076ULL},
    {0xf951a7ff43de8c79ULL, 0x11bebdf578b2f391ULL},
    {0xc21c3ffed2fdad8eULL, 0x1c6463225ab7ec1cULL},
    {0x01b0333242648ad8ULL, 0x16b6b5b5155ff017ULL},
    {0x0159c28e9b83a246ULL, 0x122bc490dde659acULL},
    {0xcef604175f3903a3ULL, 0x1d12d41afca3c2acULL},
    {0x725e69ac4c2d9c83ULL, 0x17424348ca1c9bbdULL},
    {0xf5185489d68ae39cULL, 0x129b69070816e2fdULL},
    {0xee8d540fbdab05c6ULL, 0x1dc574d80cf16b2fULL},
    {0xbed77672fe226b05ULL, 0x17d12a4670c1228cULL},
    {0xff12c528cb4ebc04ULL, 0x130dbb6b8d674ed6ULL},
    {0xcb513b74787df9a0ULL, 0x1e7c5f127bd87e24ULL},
    {0x090dc929f9fe614dULL, 0x18637f41fcad31b7ULL},
    {0xa0d7d42194cb810aULL, 0x1382cc34ca2427c5ULL},
    {0x67bfb9cf5478ce77ULL, 0x1f37ad21436d0c6fULL},
    {0x1fcc94a5dd2d71f9ULL, 0x18f9574dcf8a7059ULL},
    {0x7fd6dd517dbdf4c7ULL, 0x13faac3e3fa1f37aULL},
    {0xffbe2ee8c92fee0bULL, 0x1ff779fd329cb8c3ULL},
    {0x6631bf20a0f324d6ULL, 0x1992c7fdc216fa36ULL},
    {0xb827cc1a1a5c1d78ULL, 0x14756ccb01abfb5eULL},
    {0x935309ae7b7ce460ULL, 0x105df0a267bcc918ULL},
    {0x1eeb42b0c594a099ULL, 0x1a2fe76a3f9474f4ULL},
    {0xe58902270476e6e1ULL, 0x14f31f8832dd2a5cULL},
    {0xb7a0ce859d2bebe7ULL, 0x10c27fa028b0eeb0ULL},
    {0x59014a6f61dfdfd8ULL, 0x1ad0cc33744e4ab4ULL},
    {0xe0cdd525e7e64cadULL, 0x1573d68f903ea229ULL},
    {0x4d7177518651d6f1ULL, 0x11297872d9cbb4eeULL},
    {0x7be8bee8d6e957e8ULL, 0x1b758d848fac54b0ULL},
    {0xfcba3253df211320ULL, 0x15f7a46a0c89dd59ULL},
    {0x63c8284318e74280ULL, 0x1192e9ee706e4aaeULL},
    {0x060d0d3827d86a66ULL, 0x1c1e43171a4a1117ULL},
    {0x6b3da42cecad21ebULL, 0x167e9c127b6e7412ULL},
    {0x88fe1cf0bd574e56ULL, 0x11fee341fc585cdbULL},
    {0x419694b462254a23ULL, 0x1ccb0536608d615fULL},
    {0x67abaa29e81dd4e9ULL, 0x1708d0f84d3de77fULL},
    {0xb95621bb2017dd87ULL, 0x126d73f9d764b932ULL},
    {0xc223692b668c95a5ULL, 0x1d7becc2f23ac1eaULL},
    {0xce82ba891ed6de1dULL, 0x179657025b6234bbULL},
    {0xa53562074bdf1818ULL, 0x12deac01e2b4f6fcULL},
    {0x3b889cd87964f359ULL, 0x1e3113363787f194ULL},
    {0xfc6d4a46c783f5e1ULL, 0x18274291c6065adcULL},
    {0x30576e9f06032b1aULL, 0x13529ba7d19eaf17ULL},
    {0x1a257dcb3cd1de90ULL, 0x1eea92a61c311825ULL},
    {0x481dfe3c30a7e540ULL, 0x18bba884e35a79b7ULL},
    {0xd34b31c9c0865100ULL, 0x13c9539d82aec7c5ULL},
    {0x5211e942cda3b4cdULL, 0x1fa885c8d117a609ULL},
    {0x74db21023e1c90a4ULL, 0x19539e3a40dfb807ULL},
    {0xf715b401cb4a0d50ULL, 0x1442e4fb67196005ULL},
    {0xf8de299b09080aa7ULL, 0x103583fc527ab337ULL},
    {0x8e304291a80cddd7ULL, 0x19ef3993b72ab859ULL},
    {0x3e8d020e200a4b13ULL, 0x14bf6142f8eef9e1ULL},
    {0x653d9b3e80083c0fULL, 0x10991a9bfa58c7e7ULL},
    {0x6ec8f864000d2ce4ULL, 0x1a8e90f9908e0ca5ULL},
    {0x8bd3f9e999a423eaULL, 0x153eda614071a3b7ULL},
    {0x3ca994bae1501cbbULL, 0x10ff151a99f482f9ULL},
    {0xc775bac49bb3612bULL, 0x1b31bb5dc320d18eULL},
    {0xd2c4956a16291a89ULL, 0x15c162b168e70e0bULL},
    {0xdbd0778811ba7ba1ULL, 0x11678227871f3e6fULL},
    {0x2c80bf401c5d929bULL, 0x1bd8d03f3e9863e6ULL},
    {0xbd33cc3349e47549ULL, 0x16470cff6546b651ULL},
    {0xca8fd68f6e505dd4ULL, 0x11d270cc51055ea7ULL},
    {0x4419574be3b3c953ULL, 0x1c83e7ad4e6efdd9ULL},
    {0x0347790982f63aa9ULL, 0x16cfec8aa52597e1ULL},
    {0xcf6c60d468c4fbbaULL, 0x123ff06eea847980ULL},
    {0xe57a34870e07f92aULL, 0x1d331a4b10d3f59aULL},
    {0x512e906c0b399422ULL, 0x175c1508da432ae2ULL},
    {0xda8ba6bcd5c7a9b5ULL, 0x12b010d3e1cf5581ULL},
    {0x90df712e22d90f87ULL, 0x1de6815302e5559cULL},
    {0xda4c5a8b4f140c6cULL, 0x17eb9aa8cf1dde16ULL},
    {0xaea37ba2a5a9a38aULL, 0x1322e220a5b17e78ULL},
    {0x7dd25f6aa2a905a9ULL, 0x1e9e369aa2b59727ULL},
    {0x97db7f888220d154ULL, 0x187e92154ef7ac1fULL},
    {0x797c6606ce80a777ULL, 0x139874ddd8c6234cULL},
    {0x8f2d700ae4010bf1ULL, 0x1f5a549627a36badULL},
    {0x0c2459a25000d65aULL, 0x191510781fb5efbeULL},
    {0x701d1481d99a4515ULL, 0x1410d9f9b2f7f2feULL},
    {0xc017439b147b6a77ULL, 0x100d7b2e28c65bfeULL},
    {0xccf205c4ed9243f2ULL, 0x19af2b7d0e0a2ccaULL},
    {0x0a5b37d0be0e9cc2ULL, 0x148c22ca71a1bd6fULL},
    {0x0848f973cb3ee3ceULL, 0x10701bd527b4978cULL},
    {0xda0e5bec78649fb0ULL, 0x1a4cf9550c5425acULL},
    {0x7b3eaff060507fc0ULL, 0x150a6110d6a9b7bdULL},
    {0x95cbbff380406633ULL, 0x10d51a73deee2c97ULL},
    {0xefac665266cd7052ULL, 0x1aee90b964b04758ULL},
    {0x2623850eb8a459dbULL, 0x158ba6fab6f36c47ULL},
    {0x1e82d0d893b6ae49ULL, 0x113c85955f29236cULL},
    {0xfd9e1af41f8ab075ULL, 0x1b9408eefea838acULL},
    {0x97b1af29b2d559f7ULL, 0x16100725988693bdULL},
    {0xac8e25baf5777b2cULL, 0x11a66c1e139edc97ULL},
    {0x7a7d092b2258c513ULL, 0x1c3d79c9b8fe2dbfULL},
    {0x61fda0ef4ead6a76ULL, 0x169794a160cb57ccULL},
    {0xe7fe1a590bbdeec5ULL, 0x1212dd4de7091309ULL},
    {0xa6635d5b45fcb13aULL, 0x1ceafbafd80e84dcULL},
    {0x851c4aaf6b308dc8ULL, 0x172262f3133ed0b0ULL},
    {0xd0e36ef2bc26d7d4ULL, 0x1281e8c275cbda26ULL},
    {0xb49f17eac6a48c86ULL, 0x1d9ca79d894629d7ULL},
    {0x2a18dfef0550706bULL, 0x17b08617a104ee46ULL},
    {0x54e0b3259dd9f389ULL, 0x12f39e794d9d8b6bULL},
    {0x87cdeb6f62f65274ULL, 0x1e5297287c2f4578ULL},
    {0xd30b22bf825ea85dULL, 0x18421286c9bf6ac6ULL},
    {0x0f3c1bcc684bb9e4ULL, 0x13680ed23aff889fULL},
    {0x18602c7a4079296dULL, 0x1f0ce4839198da98ULL},
    {0x46b356c833942124ULL, 0x18d71d360e13e213ULL},
    {0x388f78a029434db6ULL, 0x13df4a91a4dcb4dcULL},
    {0x5a7f2766a86baf8aULL, 0x1fcbaa82a1612160ULL},
    {0x153285ebb9efbfa2ULL, 0x196fbb9bb44db44dULL},
    {0xaa8ed189618c994eULL, 0x145962e2f6a4903dULL},
    {0xeed8a7a11ad6e10cULL, 0x1047824f2bb6d9caULL},
    {0x7e27729b5e249b45ULL, 0x1a0c03b1df8af611ULL},
    {0xfe85f549181d4904ULL, 0x14d6695b193bf80dULL},
    {0xcb9e5dd4134aa0d0ULL, 0x10ab877c142ff9a4ULL},
    {0xdf63c9535211014dULL, 0x1aac0bf9b9e65c3aULL},
    {0x191ca10f74da6771ULL, 0x15566ffafb1eb02fULL},
    {0xadb080d92a4852c1ULL, 0x1111f32f2f4bc025ULL},
    {0x15e7348eaa0d5134ULL, 0x1b4feb7eb212cd09ULL},
    {0xab1f5d3eee710dc4ULL, 0x15d98932280f0a6dULL},
    {0xbc1917658b8da49dULL, 0x117ad428200c0857ULL},
    {0x2cf4f23c127c3a94ULL, 0x1bf7b9d9cce00d59ULL},
    {0xf0c3f4fcdb969543ULL, 0x165fc7e170b33de0ULL},
    {0x5a365d9716121103ULL, 0x11e6398126f5cb1aULL},
    {0x9056fc24f01ce804ULL, 0x1ca38f350b22de90ULL},
    {0xd9df301d8ce3ecd0ULL, 0x16e93f5da2824ba6ULL},
    {0xe17f59b13d8323daULL, 0x125432b14ecea2ebULL},
    {0x68cbc2b52f38395cULL, 0x1d53844ee47dd179ULL},
    {0x53d6355dbf602de3ULL, 0x177603725064a794ULL},
    {0xa9782ab165e68b1cULL, 0x12c4cf8ea6b6ec76ULL},
    {0x0f26aab56fd744faULL, 0x1e07b27dd78b13f1ULL},
    {0x3f52222abfdf6a62ULL, 0x18062864ac6f4327ULL},
    {0x65db4e88997f884eULL, 0x1338205089f29c1fULL},
    {0x6fc54a7428cc0d4aULL, 0x1ec033b40fea9365ULL},
    {0x596aa1f68709a43bULL, 0x1899c2f673220f84ULL},
    {0xadeee7f86c07b696ULL, 0x13ae3591f5b4d936ULL},
    {0x497e3ff3e00c5756ULL, 0x1f7d228322baf524ULL},
    {0xd464fff64cd6ac45ULL, 0x1930e868e89590e9ULL},
    {0x4383fff83d7889d1ULL, 0x14272053ed4473eeULL},
    {0xcf9cccc69793a174ULL, 0x101f4d0ff1038ff1ULL},
    {0x7f6147a425b90252ULL, 0x19cbae7fe805b31cULL},
    {0xcc4dd2e9b7c7350fULL, 0x14a2f1ffecd15c16ULL},
    {0x3d0b0f215fd290d9ULL, 0x10825b3323dab012ULL},
    {0x61ab4b689950e7c1ULL, 0x1a6a2b85062ab350ULL},
    {0x4e22a2ba1440b967ULL, 0x1521bc6a6b555c40ULL},
    {0x0b4ee894dd009453ULL, 0x10e7c9eebc4449cdULL},
    {0x1217da87c800ed51ULL, 0x1b0c764ac6d3a948ULL},
    {0xdb46486ca000bddaULL, 0x15a391d56bdc876cULL},
    {0x490506bd4ccd64afULL, 0x114fa7ddefe39f8aULL},
    {0xa8080ac87ae23ab1ULL, 0x1bb2a62fe638ff43ULL},
    {0x5339a239fbe82ef4ULL, 0x162884f31e93ff69ULL},
    {0x75c7b4fb2fecf25dULL, 0x11ba03f5b20fff87ULL},
    {0x22d92191e647ea2eULL, 0x1c5cd322b67fff3fULL},
    {0xb57a8141850654f2ULL, 0x16b0a8e891ffff65ULL},
    {0xc4620101373843f5ULL, 0x1226ed86db3332b7ULL},
    {0x3a366801f1f39feeULL, 0x1d0b15a491eb8459ULL},
    {0xfb5eb99b27f6198bULL, 0x173c115074bc69e0ULL},
    {0x2f7efae2865e7ad6ULL, 0x129674405d6387e7ULL},
    {0xe597f7d0d6fd9156ULL, 0x1dbd86cd6238d971ULL},
    {0x8479930d78cadaabULL, 0x17cad23de82d7ac1ULL},
    {0xd06142712d6f1556ULL, 0x1308a831868ac89aULL},
    {0x4d686a4eaf182222ULL, 0x1e74404f3daada91ULL},
    {0xa453883ef279b4e8ULL, 0x185d003f6488aedaULL},
    {0xe9dc6cff28615d87ULL, 0x137d99cc506d58aeULL},
    {0xa960ae650d6895a4ULL, 0x1f2f5c7a1a488de4ULL},
    {0xbab3beb73ded4483ULL, 0x18f2b061aea07183ULL},
    {0x2ef6322c318a9d36ULL, 0x13f559e7bee6c136ULL},
};

static const uint64_t RYU_DOUBLE_POW5_SPLIT[326][2] = {
    {0x0000000000000000ULL, 0x1000000000000000ULL},
    {0x0000000000000000ULL, 0x1400000000000000ULL},
    {0x0000000000000000ULL, 0x1900000000000000ULL},
    {0x0000000000000000ULL, 0x1f40000000000000ULL},
    {0x0000000000000000ULL, 0x1388000000000000ULL},
    {0x0000000000000000ULL, 0x186a000000000000ULL},
    {0x0000000000000000ULL, 0x1e84800000000000ULL},
    {0x0000000000000000ULL, 0x1312d00000000000ULL},
    {0x0000000000000000ULL, 0x17d7840000000000ULL},
    {0x0000000000000000ULL, 0x1dcd650000000000ULL},
    {0x0000000000000000ULL, 0x12a05f2000000000ULL},
    {0x0000000000000000ULL, 0x174876e800000000ULL},
    {0x0000000000000000ULL, 0x1d1a94a200000000ULL},
    {0x0000000000000000ULL, 0x12309ce540000000ULL},
    {0x0000000000000000ULL, 0x16bcc41e90000000ULL},
    {0x0000000000000000ULL, 0x1c6bf52634000000ULL},
    {0x0000000000000000ULL, 0x11c37937e0800000ULL},
    {0x0000000000000000ULL, 0x16345785d8a00000ULL},
    {0x0000000000000000ULL, 0x1bc16d674ec80000ULL},
    {0x0000000000000000ULL, 0x1158e460913d0000ULL},
    {0x0000000000000000ULL, 0x15af1d78b58c4000ULL},
    {0x0000000000000000ULL, 0x1b1ae4d6e2ef5000ULL},
    {0x0000000000000000ULL, 0x10f0cf064dd59200ULL},
    {0x0000000000000000ULL, 0x152d02c7e14af680ULL},
    {0x0000000000000000ULL, 0x1a784379d99db420ULL},
    {0x0000000000000000ULL, 0x108b2a2c28029094ULL},
    {0x0000000000000000ULL, 0x14adf4b7320334b9ULL},
    {0x4000000000000000ULL, 0x19d971e4fe8401e7ULL},
    {0x8800000000000000ULL, 0x1027e72f1f128130ULL},
    {0xaa00000000000000ULL, 0x1431e0fae6d7217cULL},
    {0xd480000000000000ULL, 0x193e5939a08ce9dbULL},
    {0xc9a0000000000000ULL, 0x1f8def8808b02452ULL},
    {0xbe04000000000000ULL, 0x13b8b5b5056e16b3ULL},
    {0xad85000000000000ULL, 0x18a6e32246c99c60ULL},
    {0xd8e6400000000000ULL, 0x1ed09bead87c0378ULL},
    {0x878fe80000000000ULL, 0x13426172c74d822bULL},
    {0x6973e20000000000ULL, 0x1812f9cf7920e2b6ULL},
    {0x03d0da8000000000ULL, 0x1e17b84357691b64ULL},
    {0x8262889000000000ULL, 0x12ced32a16a1b11eULL},
    {0x22fb2ab400000000ULL, 0x178287f49c4a1d66ULL},
    {0xabb9f56100000000ULL, 0x1d6329f1c35ca4bfULL},
    {0xcb54395ca0000000ULL, 0x125dfa371a19e6f7ULL},
    {0xbe2947b3c8000000ULL, 0x16f578c4e0a060b5ULL},
    {0x2db399a0ba000000ULL, 0x1cb2d6f618c878e3ULL},
    {0xfc90400474400000ULL, 0x11efc659cf7d4b8dULL},
    {0x7bb4500591500000ULL, 0x166bb7f0435c9e71ULL},
    {0xdaa16406f5a40000ULL, 0x1c06a5ec5433c60dULL},
    {0xa8a4de8459868000ULL, 0x118427b3b4a05bc8ULL},
    {0xd2ce16256fe82000ULL, 0x15e531a0a1c872baULL},
    {0x87819baecbe22800ULL, 0x1b5e7e08ca3a8f69ULL},
    {0xf4b1014d3f6d5900ULL, 0x111b0ec57e6499a1ULL},
    {0x71dd41a08f48af40ULL, 0x1561d276ddfdc00aULL},
    {0x0e549208b31adb10ULL, 0x1aba4714957d300dULL},
    {0x28f4db456ff0c8eaULL, 0x10b46c6cdd6e3e08ULL},
    {0x33321216cbecfb24ULL, 0x14e1878814c9cd8aULL},
    {0xbffe969c7ee839edULL, 0x1a19e96a19fc40ecULL},
    {0xf7ff1e21cf512434ULL, 0x105031e2503da893ULL},
    {0xf5fee5aa43256d41ULL, 0x14643e5ae44d12b8ULL},
    {0x337e9f14d3eec892ULL, 0x197d4df19d605767ULL},
    {0x005e46da08ea7ab6ULL, 0x1fdca16e04b86d41ULL},
    {0xa03aec4845928cb2ULL, 0x13e9e4e4c2f34448ULL},
    {0xc849a75a56f72fdeULL, 0x18e45e1df3b0155aULL},
    {0x7a5c1130ecb4fbd6ULL, 0x1f1d75a5709c1ab1ULL},
    {0xec798abe93f11d65ULL, 0x13726987666190aeULL},
    {0xa797ed6e38ed64bfULL, 0x184f03e93ff9f4daULL},
    {0x517de8c9c728bdefULL, 0x1e62c4e38ff87211ULL},
    {0xd2eeb17e1c7976b5ULL, 0x12fdbb0e39fb474aULL},
    {0x87aa5ddda397d462ULL, 0x17bd29d1c87a191dULL},
    {0xe994f5550c7dc97bULL, 0x1dac74463a989f64ULL},
    {0x11fd195527ce9dedULL, 0x128bc8abe49f639fULL},
    {0xd67c5faa71c24568ULL, 0x172ebad6ddc73c86ULL},
    {0x8c1b77950e32d6c2ULL, 0x1cfa698c95390ba8ULL},
    {0x57912abd28dfc639ULL, 0x121c81f7dd43a749ULL},
    {0xad75756c7317b7c8ULL, 0x16a3a275d494911bULL},
    {0x98d2d2c78fdda5baULL, 0x1c4c8b1349b9b562ULL},
    {0x9f83c3bcb9ea8794ULL, 0x11afd6ec0e14115dULL},
    {0x0764b4abe8652979ULL, 0x161bcca7119915b5ULL},
    {0x493de1d6e27e73d7ULL, 0x1ba2bfd0d5ff5b22ULL},
    {0x6dc6ad264d8f0866ULL, 0x1145b7e285bf98f5ULL},
    {0xc938586fe0f2ca80ULL, 0x159725db272f7f32ULL},
    {0x7b866e8bd92f7d20ULL, 0x1afcef51f0fb5effULL},
    {0xad34051767bdae34ULL, 0x10de1593369d1b5fULL},
    {0x9881065d41ad19c1ULL, 0x15159af804446237ULL},
    {0x7ea147f492186032ULL, 0x1a5b01b605557ac5ULL},
    {0x6f24ccf8db4f3c1fULL, 0x1078e111c3556cbbULL},
    {0x4aee003712230b27ULL, 0x14971956342ac7eaULL},
    {0xdda98044d6abcdf0ULL, 0x19bcdfabc13579e4ULL},
    {0x0a89f02b062b60b6ULL, 0x10160bcb58c16c2fULL},
    {0xcd2c6c35c7b638e4ULL, 0x141b8ebe2ef1c73aULL},
    {0x8077874339a3c71dULL, 0x1922726dbaae3909ULL},
    {0xe0956914080cb8e4ULL, 0x1f6b0f092959c74bULL},
    {0x6c5d61ac8507f38eULL, 0x13a2e965b9d81c8fULL},
    {0x4774ba17a649f072ULL, 0x188ba3bf284e23b3ULL},
    {0x1951e89d8fdc6c8fULL, 0x1eae8caef261aca0ULL},
    {0x0fd3316279e9c3d9ULL, 0x132d17ed577d0be4ULL},
    {0x13c7fdbb186434cfULL, 0x17f85de8ad5c4eddULL},
    {0x58b9fd29de7d4203ULL, 0x1df67562d8b36294ULL},
    {0xb7743e3a2b0e4942ULL, 0x12ba095dc7701d9cULL},
    {0xe5514dc8b5d1db92ULL, 0x17688bb5394c2503ULL},
    {0xdea5a13ae3465277ULL, 0x1d42aea2879f2e44ULL},
    {0x0b2784c4ce0bf38aULL, 0x1249ad2594c37cebULL},
    {0xcdf165f6018ef06dULL, 0x16dc186ef9f45c25ULL},
    {0x416dbf7381f2ac88ULL, 0x1c931e8ab871732fULL},
    {0x88e497a83137abd5ULL, 0x11dbf316b346e7fdULL},
    {0xeb1dbd923d8596caULL, 0x1652efdc6018a1fcULL},
    {0x25e52cf6cce6fc7dULL, 0x1be7abd3781eca7cULL},
    {0x97af3c1a40105dceULL, 0x1170cb642b133e8dULL},
    {0xfd9b0b20d0147542ULL, 0x15ccfe3d35d80e30ULL},
    {0x3d01cde904199292ULL, 0x1b403dcc834e11bdULL},
    {0x462120b1a28ffb9bULL, 0x1108269fd210cb16ULL},
    {0xd7a968de0b33fa82ULL, 0x154a3047c694fddbULL},
    {0xcd93c3158e00f923ULL, 0x1a9cbc59b83a3d52ULL},
    {0xc07c59ed78c09bb6ULL, 0x10a1f5b813246653ULL},
    {0xb09b7068d6f0c2a3ULL, 0x14ca732617ed7fe8ULL},
    {0xdcc24c830cacf34cULL, 0x19fd0fef9de8dfe2ULL},
    {0xc9f96fd1e7ec180fULL, 0x103e29f5c2b18bedULL},
    {0x3c77cbc661e71e13ULL, 0x144db473335deee9ULL},
    {0x8b95beb7fa60e598ULL, 0x1961219000356aa3ULL},
    {0x6e7b2e65f8f91efeULL, 0x1fb969f40042c54cULL},
    {0xc50cfcffbb9bb35fULL, 0x13d3e2388029bb4fULL},
    {0xb6503c3faa82a037ULL, 0x18c8dac6a0342a23ULL},
    {0xa3e44b4f95234844ULL, 0x1efb1178484134acULL},
    {0xe66eaf11bd360d2bULL, 0x135ceaeb2d28c0ebULL},
    {0xe00a5ad62c839075ULL, 0x183425a5f872f126ULL},
    {0x980cf18bb7a47493ULL, 0x1e412f0f768fad70ULL},
    {0x5f0816f752c6c8dcULL, 0x12e8bd69aa19cc66ULL},
    {0xf6ca1cb527787b13ULL, 0x17a2ecc414a03f7fULL},
    {0xf47ca3e2715699d7ULL, 0x1d8ba7f519c84f5fULL},
    {0xf8cde66d86d62026ULL, 0x127748f9301d319bULL},
    {0xf7016008e88ba830ULL, 0x17151b377c247e02ULL},
    {0xb4c1b80b22ae923cULL, 0x1cda62055b2d9d83ULL},
    {0x50f91306f5ad1b65ULL, 0x12087d4358fc8272ULL},
    {0xe53757c8b318623fULL, 0x168a9c942f3ba30eULL},
    {0x9e852dbadfde7acfULL, 0x1c2d43b93b0a8bd2ULL},
    {0xa3133c94cbeb0cc1ULL, 0x119c4a53c4e69763ULL},
    {0x8bd80bb9fee5cff1ULL, 0x16035ce8b6203d3cULL},
    {0xaece0ea87e9f43eeULL, 0x1b843422e3a84c8bULL},
    {0x4d40c9294f238a75ULL, 0x1132a095ce492fd7ULL},
    {0x2090fb73a2ec6d12ULL, 0x157f48bb41db7bcdULL},
    {0x68b53a508ba78856ULL, 0x1adf1aea12525ac0ULL},
    {0x417144725748b536ULL, 0x10cb70d24b7378b8ULL},
    {0x51cd958eed1ae283ULL, 0x14fe4d06de5056e6ULL},
    {0xe640faf2a8619b24ULL, 0x1a3de04895e46c9fULL},
    {0xefe89cd7a93d00f7ULL, 0x1066ac2d5daec3e3ULL},
    {0xebe2c40d938c4134ULL, 0x14805738b51a74dcULL},
    {0x26db7510f86f5181ULL, 0x19a06d06e2611214ULL},
    {0x9849292a9b4592f1ULL, 0x100444244d7cab4cULL},
    {0xbe5b73754216f7adULL, 0x1405552d60dbd61fULL},
    {0xadf25052929cb598ULL, 0x1906aa78b912cba7ULL},
    {0x996ee4673743e2ffULL, 0x1f485516e7577e91ULL},
    {0xffe54ec0828a6ddfULL, 0x138d352e5096af1aULL},
    {0xbfdea270a32d0957ULL, 0x18708279e4bc5ae1ULL},
    {0x2fd64b0ccbf84badULL, 0x1e8ca3185deb719aULL},
    {0x5de5eee7ff7b2f4cULL, 0x1317e5ef3ab32700ULL},
    {0x755f6aa1ff59fb1fULL, 0x17dddf6b095ff0c0ULL},
    {0x92b7454a7f3079e7ULL, 0x1dd55745cbb7ecf0ULL},
    {0x5bb28b4e8f7e4c30ULL, 0x12a5568b9f52f416ULL},
    {0xf29f2e22335ddf3cULL, 0x174eac2e8727b11bULL},
    {0xef46f9aac035570bULL, 0x1d22573a28f19d62ULL},
    {0xd58c5c0ab8215667ULL, 0x123576845997025dULL},
    {0x4aef730d6629ac01ULL, 0x16c2d4256ffcc2f5ULL},
    {0x9dab4fd0bfb41701ULL, 0x1c73892ecbfbf3b2ULL},
    {0xa28b11e277d08e60ULL, 0x11c835bd3f7d784fULL},
    {0x8b2dd65b15c4b1f9ULL, 0x163a432c8f5cd663ULL},
    {0x6df94bf1db35de77ULL, 0x1bc8d3f7b3340bfcULL},
    {0xc4bbcf772901ab0aULL, 0x115d847ad000877dULL},
    {0x35eac354f34215cdULL, 0x15b4e5998400a95dULL},
    {0x8365742a30129b40ULL, 0x1b221effe500d3b4ULL},
    {0xd21f689a5e0ba108ULL, 0x10f5535fef208450ULL},
    {0x06a742c0f58e894aULL, 0x1532a837eae8a565ULL},
    {0x4851137132f22b9dULL, 0x1a7f5245e5a2cebeULL},
    {0xed32ac26bfd75b42ULL, 0x108f936baf85c136ULL},
    {0xa87f57306fcd3212ULL, 0x14b378469b673184ULL},
    {0xd29f2cfc8bc07e97ULL, 0x19e056584240fde5ULL},
    {0xa3a37c1dd7584f1eULL, 0x102c35f729689eafULL},
    {0x8c8c5b254d2e62e6ULL, 0x14374374f3c2c65bULL},
    {0x6faf71eea079fb9fULL, 0x1945145230b377f2ULL},
    {0x0b9b4e6a48987a87ULL, 0x1f965966bce055efULL},
    {0x674111026d5f4c94ULL, 0x13bdf7e0360c35b5ULL},
    {0xc111554308b71fbaULL, 0x18ad75d8438f4322ULL},
    {0x7155aa93cae4e7a8ULL, 0x1ed8d34e547313ebULL},
    {0x26d58a9c5ecf10c9ULL, 0x13478410f4c7ec73ULL},
    {0xf08aed437682d4fbULL, 0x1819651531f9e78fULL},
    {0xecada89454238a3aULL, 0x1e1fbe5a7e786173ULL},
    {0x73ec895cb4963664ULL, 0x12d3d6f88f0b3ce8ULL},
    {0x90e7abb3e1bbc3fdULL, 0x1788ccb6b2ce0c22ULL},
    {0x352196a0da2ab4fdULL, 0x1d6affe45f818f2bULL},
    {0x0134fe24885ab11eULL, 0x1262dfeebbb0f97bULL},
    {0xc1823dadaa715d65ULL, 0x16fb97ea6a9d37d9ULL},
    {0x31e2cd19150db4bfULL, 0x1cba7de5054485d0ULL},
    {0x1f2dc02fad2890f7ULL, 0x11f48eaf234ad3a2ULL},
    {0xa6f9303b9872b535ULL, 0x1671b25aec1d888aULL},
    {0x50b77c4a7e8f6282ULL, 0x1c0e1ef1a724eaadULL},
    {0x5272adae8f199d91ULL, 0x1188d357087712acULL},
    {0x670f591a32e004f6ULL, 0x15eb082cca94d757ULL},
    {0x40d32f60bf980633ULL, 0x1b65ca37fd3a0d2dULL},
    {0x4883fd9c77bf03e0ULL, 0x111f9e62fe44483cULL},
    {0x5aa4fd0395aec4d8ULL, 0x156785fbbdd55a4bULL},
    {0x314e3c447b1a760eULL, 0x1ac1677aad4ab0deULL},
    {0xded0e5aaccf089c9ULL, 0x10b8e0acac4eae8aULL},
    {0x96851f15802cac3bULL, 0x14e718d7d7625a2dULL},
    {0xfc2666dae037d74aULL, 0x1a20df0dcd3af0b8ULL},
    {0x9d980048cc22e68eULL, 0x10548b68a044d673ULL},
    {0x84fe005aff2ba032ULL, 0x1469ae42c8560c10ULL},
    {0xa63d8071bef6883eULL, 0x198419d37a6b8f14ULL},
    {0xcfcce08e2eb42a4eULL, 0x1fe52048590672d9ULL},
    {0x21e00c58dd309a70ULL, 0x13ef342d37a407c8ULL},
    {0x2a580f6f147cc10dULL, 0x18eb0138858d09baULL},
    {0xb4ee134ad99bf150ULL, 0x1f25c186a6f04c28ULL},
    {0x7114cc0ec80176d2ULL, 0x137798f428562f99ULL},
    {0xcd59ff127a01d486ULL, 0x18557f31326bbb7fULL},
    {0xc0b07ed7188249a8ULL, 0x1e6adefd7f06aa5fULL},
    {0xd86e4f466f516e09ULL, 0x1302cb5e6f642a7bULL},
    {0xce89e3180b25c98bULL, 0x17c37e360b3d351aULL},
    {0x822c5bde0def3beeULL, 0x1db45dc38e0c8261ULL},
    {0xf15bb96ac8b58575ULL, 0x1290ba9a38c7d17cULL},
    {0x2db2a7c57ae2e6d2ULL, 0x1734e940c6f9c5dcULL},
    {0x391f51b6d99ba086ULL, 0x1d022390f8b83753ULL},
    {0x03b3931248014454ULL, 0x1221563a9b732294ULL},
    {0x04a077d6da019569ULL, 0x16a9abc9424feb39ULL},
    {0x45c895cc9081fac3ULL, 0x1c5416bb92e3e607ULL},
    {0x8b9d5d9fda513cbaULL, 0x11b48e353bce6fc4ULL},
    {0xae84b507d0e58be8ULL, 0x1621b1c28ac20bb5ULL},
    {0x1a25e249c51eeee3ULL, 0x1baa1e332d728ea3ULL},
    {0xf057ad6e1b33554dULL, 0x114a52dffc679925ULL},
    {0x6c6d98c9a2002aa1ULL, 0x159ce797fb817f6fULL},
    {0x4788fefc0a803549ULL, 0x1b04217dfa61df4bULL},
    {0x0cb59f5d8690214eULL, 0x10e294eebc7d2b8fULL},
    {0xcfe30734e83429a1ULL, 0x151b3a2a6b9c7672ULL},
    {0x83dbc9022241340aULL, 0x1a6208b50683940fULL},
    {0xb2695da15568c086ULL, 0x107d457124123c89ULL},
    {0x1f03b509aac2f0a7ULL, 0x149c96cd6d16cbacULL},
    {0x26c4a24c1573acd1ULL, 0x19c3bc80c85c7e97ULL},
    {0x783ae56f8d684c03ULL, 0x101a55d07d39cf1eULL},
    {0x16499ecb70c25f03ULL, 0x1420eb449c8842e6ULL},
    {0x9bdc067e4cf2f6c4ULL, 0x19292615c3aa539fULL},
    {0x82d3081de02fb476ULL, 0x1f736f9b3494e887ULL},
    {0xb1c3e512ac1dd0c9ULL, 0x13a825c100dd1154ULL},
    {0xde34de57572544fcULL, 0x18922f31411455a9ULL},
    {0x55c215ed2cee963bULL, 0x1eb6bafd91596b14ULL},
    {0xb5994db43c151de5ULL, 0x133234de7ad7e2ecULL},
    {0xe2ffa1214b1a655eULL, 0x17fec216198ddba7ULL},
    {0xdbbf89699de0feb6ULL, 0x1dfe729b9ff15291ULL},
    {0x2957b5e202ac9f31ULL, 0x12bf07a143f6d39bULL},
    {0xf3ada35a8357c6feULL, 0x176ec98994f48881ULL},
    {0x70990c31242db8bdULL, 0x1d4a7bebfa31aaa2ULL},
    {0x865fa79eb69c9376ULL, 0x124e8d737c5f0aa5ULL},
    {0xe7f791866443b854ULL, 0x16e230d05b76cd4eULL},
    {0xa1f575e7fd54a669ULL, 0x1c9abd04725480a2ULL},
    {0xa53969b0fe54e801ULL, 0x11e0b622c774d065ULL},
    {0x0e87c41d3dea2202ULL, 0x1658e3ab7952047fULL},
    {0xd229b5248d64aa82ULL, 0x1bef1c9657a6859eULL},
    {0x435a1136d85eea91ULL, 0x117571ddf6c81383ULL},
    {0x143095848e76a536ULL, 0x15d2ce55747a1864ULL},
    {0x193cbae5b2144e83ULL, 0x1b4781ead1989e7dULL},
    {0x2fc5f4cf8f4cb112ULL, 0x110cb132c2ff630eULL},
    {0xbbb77203731fdd56ULL, 0x154fdd7f73bf3bd1ULL},
    {0x2aa54e844fe7d4acULL, 0x1aa3d4df50af0ac6ULL},
    {0xdaa75112b1f0e4ebULL, 0x10a6650b926d66bbULL},
    {0xd15125575e6d1e26ULL, 0x14cffe4e7708c06aULL},
    {0x85a56ead360865b0ULL, 0x1a03fde214caf085ULL},
    {0x7387652c41c53f8eULL, 0x10427ead4cfed653ULL},
    {0x50693e7752368f71ULL, 0x14531e58a03e8be8ULL},
    {0x64838e1526c4334eULL, 0x1967e5eec84e2ee2ULL},
    {0xfda4719a70754022ULL, 0x1fc1df6a7a61ba9aULL},
    {0xde86c70086494815ULL, 0x13d92ba28c7d14a0ULL},
    {0x162878c0a7db9a1aULL, 0x18cf768b2f9c59c9ULL},
    {0x5bb296f0d1d280a1ULL, 0x1f03542dfb83703bULL},
    {0x194f9e5683239064ULL, 0x1362149cbd322625ULL},
    {0x5fa385ec23ec747eULL, 0x183a99c3ec7eafaeULL},
    {0xf78c67672ce7919dULL, 0x1e494034e79e5b99ULL},
    {0x3ab7c0a07c10bb02ULL, 0x12edc82110c2f940ULL},
    {0x4965b0c89b14e9c3ULL, 0x17a93a2954f3b790ULL},
    {0x5bbf1cfac1da2433ULL, 0x1d9388b3aa30a574ULL},
    {0xb957721cb92856a0ULL, 0x127c35704a5e6768ULL},
    {0xe7ad4ea3e7726c48ULL, 0x171b42cc5cf60142ULL},
    {0xa198a24ce14f075aULL, 0x1ce2137f74338193ULL},
    {0x44ff65700cd16498ULL, 0x120d4c2fa8a030fcULL},
    {0x563f3ecc1005bdbeULL, 0x16909f3b92c83d3bULL},
    {0x2bcf0e7f14072d2eULL, 0x1c34c70a777a4c8aULL},
    {0x5b61690f6c847c3dULL, 0x11a0fc668aac6fd6ULL},
    {0xf239c35347a59b4cULL, 0x16093b802d578bcbULL},
    {0xeec83428198f021fULL, 0x1b8b8a6038ad6ebeULL},
    {0x553d20990ff96153ULL, 0x1137367c236c6537ULL},
    {0x2a8c68bf53f7b9a8ULL, 0x1585041b2c477e85ULL},
    {0x752f82ef28f5a812ULL, 0x1ae64521f7595e26ULL},
    {0x093db1d57999890bULL, 0x10cfeb353a97dad8ULL},
    {0x0b8d1e4ad7ffeb4eULL, 0x1503e602893dd18eULL},
    {0x8e7065dd8dffe622ULL, 0x1a44df832b8d45f1ULL},
    {0xf9063faa78bfefd5ULL, 0x106b0bb1fb384bb6ULL},
    {0xb747cf9516efebcaULL, 0x1485ce9e7a065ea4ULL},
    {0xe519c37a5cabe6bdULL, 0x19a742461887f64dULL},
    {0xaf301a2c79eb7036ULL, 0x1008896bcf54f9f0ULL},
    {0xdafc20b798664c43ULL, 0x140aabc6c32a386cULL},
    {0x11bb28e57e7fdf54ULL, 0x190d56b873f4c688ULL},
    {0x1629f31ede1fd72aULL, 0x1f50ac6690f1f82aULL},
    {0x4dda37f34ad3e67aULL, 0x13926bc01a973b1aULL},
    {0xe150c5f01d88e019ULL, 0x187706b0213d09e0ULL},
    {0x19a4f76c24eb181fULL, 0x1e94c85c298c4c59ULL},
    {0xb0071aa39712ef13ULL, 0x131cfd3999f7afb7ULL},
    {0x9c08e14c7cd7aad8ULL, 0x17e43c8800759ba5ULL},
    {0x030b199f9c0d958eULL, 0x1ddd4baa0093028fULL},
    {0x61e6f003c1887d79ULL, 0x12aa4f4a405be199ULL},
    {0xba60ac04b1ea9cd7ULL, 0x1754e31cd072d9ffULL},
    {0xa8f8d705de65440dULL, 0x1d2a1be4048f907fULL},
    {0xc99b8663aaff4a88ULL, 0x123a516e82d9ba4fULL},
    {0xbc0267fc95bf1d2aULL, 0x16c8e5ca239028e3ULL},
    {0xab0301fbbb2ee474ULL, 0x1c7b1f3cac74331cULL},
    {0xeae1e13d54fd4ec9ULL, 0x11ccf385ebc89ff1ULL},
    {0x659a598caa3ca27bULL, 0x1640306766bac7eeULL},
    {0xff00efefd4cbcb1aULL, 0x1bd03c81406979e9ULL},
    {0x3f6095f5e4ff5ef0ULL, 0x116225d0c841ec32ULL},
    {0xcf38bb735e3f36acULL, 0x15baaf44fa52673eULL},
    {0x8306ea5035cf0457ULL, 0x1b295b1638e7010eULL},
    {0x11e4527221a162b6ULL, 0x10f9d8ede39060a9ULL},
    {0x565d670eaa09bb64ULL, 0x15384f295c7478d3ULL},
    {0x2bf4c0d2548c2a3dULL, 0x1a8662f3b3919708ULL},
    {0x1b78f88374d79a66ULL, 0x1093fdd8503afe65ULL},
    {0x625736a4520d8100ULL, 0x14b8fd4e6449bdfeULL},
    {0xfaed044d6690e140ULL, 0x19e73ca1fd5c2d7dULL},
    {0xbcd422b0601a8cc8ULL, 0x103085e53e599c6eULL},
    {0x6c092b5c78212ffaULL, 0x143ca75e8df0038aULL},
    {0x070b763396297bf8ULL, 0x194bd136316c046dULL},
    {0x48ce53c07bb3daf6ULL, 0x1f9ec583bdc70588ULL},
    {0x2d80f4584d5068daULL, 0x13c33b72569c6375ULL},
    {0x78e1316e60a48310ULL, 0x18b40a4eec437c52ULL},
};

#define RYU_DOUBLE_MANTISSA_BITS 52
#define RYU_DOUBLE_EXPONENT_BITS 11
#define RYU_DOUBLE_BIAS 1023
#define RYU_DOUBLE_POW5_INV_BITCOUNT 125
#define RYU_DOUBLE_POW5_BITCOUNT 125

static inline uint32_t ryu_pow5factor_2(uint64_t value)
{
    uint32_t count = 0;
    for (;;) {
        uint64_t q = value / 5ULL;
        uint64_t r = value % 5ULL;
        if (r != 0ULL) {
            break;
        }
        value = q;
        count++;
    }
    return count;
}

static inline bool ryu_multiple_of_power_of_5(uint64_t value, uint32_t p)
{
    return ryu_pow5factor_2(value) >= p;
}

static inline bool ryu_multiple_of_power_of_2(uint64_t value, uint32_t p)
{
    if (p >= 64u) {
        return value == 0ULL;
    }
    return (value & ((1ULL << p) - 1ULL)) == 0ULL;
}

static inline uint32_t ryu_log10_pow2(int32_t e)
{
    return (uint32_t)((((uint32_t)e) * 78913u) >> 18);
}

static inline uint32_t ryu_log10_pow5(int32_t e)
{
    return (uint32_t)((((uint32_t)e) * 732923u) >> 20);
}

static inline int32_t ryu_pow5bits(int32_t e)
{
    return (e == 0) ? 1 : (int32_t)(((((uint32_t)e) * 1217359u) >> 19) + 1u);
}

static inline uint32_t ryu_decimal_length17(uint64_t v)
{
    if (v >= 10000000000000000ULL) {
        return 17u;
    }
    if (v >= 1000000000000000ULL) {
        return 16u;
    }
    if (v >= 100000000000000ULL) {
        return 15u;
    }
    if (v >= 10000000000000ULL) {
        return 14u;
    }
    if (v >= 1000000000000ULL) {
        return 13u;
    }
    if (v >= 100000000000ULL) {
        return 12u;
    }
    if (v >= 10000000000ULL) {
        return 11u;
    }
    if (v >= 1000000000ULL) {
        return 10u;
    }
    if (v >= 100000000ULL) {
        return 9u;
    }
    if (v >= 10000000ULL) {
        return 8u;
    }
    if (v >= 1000000ULL) {
        return 7u;
    }
    if (v >= 100000ULL) {
        return 6u;
    }
    if (v >= 10000ULL) {
        return 5u;
    }
    if (v >= 1000ULL) {
        return 4u;
    }
    if (v >= 100ULL) {
        return 3u;
    }
    if (v >= 10ULL) {
        return 2u;
    }
    return 1u;
}

static inline uint64_t ryu_mul_shift_64(uint64_t m, const uint64_t *mul, int32_t j)
{
#if defined(__SIZEOF_INT128__)
    __uint128_t b0 = (__uint128_t)m * mul[0];
    __uint128_t b2 = (__uint128_t)m * mul[1];
    __uint128_t sum = (b0 >> 64) + b2;
    return (uint64_t)(sum >> (j - 64));
#else
    uint64_t m_lo = (uint32_t)m;
    uint64_t m_hi = m >> 32;
    uint64_t a0 = (uint32_t)mul[0];
    uint64_t a1 = mul[0] >> 32;
    uint64_t b0_ll = m_lo * a0;
    uint64_t b0_lh = m_lo * a1;
    uint64_t b0_hl = m_hi * a0;
    uint64_t b0_hh = m_hi * a1;
    uint64_t b0_mid = (b0_ll >> 32) + (uint32_t)b0_lh + b0_hl;
    uint64_t b0_hi = (b0_lh >> 32) + (b0_mid >> 32) + b0_hh;

    uint64_t c0 = (uint32_t)mul[1];
    uint64_t c1 = mul[1] >> 32;
    uint64_t b2_ll = m_lo * c0;
    uint64_t b2_lh = m_lo * c1;
    uint64_t b2_hl = m_hi * c0;
    uint64_t b2_hh = m_hi * c1;
    uint64_t b2_mid = (b2_ll >> 32) + (uint32_t)b2_lh + b2_hl;
    uint64_t b2_lo = (b2_ll & 0xFFFFFFFFULL) | ((b2_mid & 0xFFFFFFFFULL) << 32);
    uint64_t b2_hi = (b2_lh >> 32) + (b2_mid >> 32) + b2_hh;

    uint64_t sum_lo = b2_lo + b0_hi;
    uint64_t sum_hi = b2_hi + (sum_lo < b2_lo ? 1ULL : 0ULL);
    int32_t shift = j - 64;
    if (shift == 0) {
        return sum_lo;
    }
    if (shift >= 64) {
        return sum_hi >> (shift - 64);
    }
    return (sum_lo >> shift) | (sum_hi << (64 - shift));
#endif
}

static inline uint64_t ryu_mul_shift_all_64(uint64_t m, const uint64_t *mul, int32_t j,
                                            uint64_t *vp, uint64_t *vm, uint32_t mm_shift)
{
    *vp = ryu_mul_shift_64(4ULL * m + 2ULL, mul, j);
    *vm = ryu_mul_shift_64(4ULL * m - 1ULL - mm_shift, mul, j);
    return ryu_mul_shift_64(4ULL * m, mul, j);
}

bool we_json_format_double(double val, char *out, size_t out_cap)
{
    if (!out || out_cap < 2u || !isfinite(val)) {
        return false;
    }

    uint64_t bits = 0;
    memcpy(&bits, &val, sizeof(bits));

    bool sign = ((bits >> 63) & 1ULL) != 0;
    uint64_t ieee_mantissa = bits & ((1ULL << RYU_DOUBLE_MANTISSA_BITS) - 1ULL);
    uint32_t ieee_exponent = (uint32_t)((bits >> RYU_DOUBLE_MANTISSA_BITS) &
                                        ((1u << RYU_DOUBLE_EXPONENT_BITS) - 1u));

    if (ieee_exponent == 0u && ieee_mantissa == 0ULL) {
        /* RFC 8785 / ECMAScript requires -0.0 to serialize identically to 0 */
        out[0] = '0';
        out[1] = '\0';
        return true;
    }

    int32_t e2;
    uint64_t m2;
    if (ieee_exponent == 0u) {
        e2 = 1 - RYU_DOUBLE_BIAS - RYU_DOUBLE_MANTISSA_BITS - 2;
        m2 = ieee_mantissa;
    } else {
        e2 = (int32_t)ieee_exponent - RYU_DOUBLE_BIAS - RYU_DOUBLE_MANTISSA_BITS - 2;
        m2 = (1ULL << RYU_DOUBLE_MANTISSA_BITS) | ieee_mantissa;
    }

    bool even = (m2 & 1ULL) == 0ULL;
    bool accept_bounds = even;
    uint64_t mv = 4ULL * m2;
    uint32_t mm_shift = (ieee_mantissa != 0ULL || ieee_exponent <= 1u) ? 1u : 0u;

    uint64_t vr = 0;
    uint64_t vp = 0;
    uint64_t vm = 0;
    int32_t e10 = 0;
    bool vm_is_trailing_zeros = false;
    bool vr_is_trailing_zeros = false;

    if (e2 >= 0) {
        uint32_t q = ryu_log10_pow2(e2) - (e2 > 3 ? 1u : 0u);
        e10 = (int32_t)q;
        int32_t k = RYU_DOUBLE_POW5_INV_BITCOUNT + ryu_pow5bits((int32_t)q) - 1;
        int32_t i = -e2 + (int32_t)q + k;
        vr = ryu_mul_shift_all_64(m2, RYU_DOUBLE_POW5_INV_SPLIT[q], i, &vp, &vm, mm_shift);
        if (q <= 21u) {
            uint32_t mv_mod5 = (uint32_t)(mv % 5ULL);
            if (mv_mod5 == 0u) {
                vr_is_trailing_zeros = ryu_multiple_of_power_of_5(mv, q);
            } else if (accept_bounds) {
                vm_is_trailing_zeros = ryu_multiple_of_power_of_5(mv - 1ULL - mm_shift, q);
            } else {
                vp -= ryu_multiple_of_power_of_5(mv + 2ULL, q) ? 1ULL : 0ULL;
            }
        }
    } else {
        uint32_t q = ryu_log10_pow5(-e2) - (-e2 > 1 ? 1u : 0u);
        e10 = (int32_t)q + e2;
        int32_t i = -e2 - (int32_t)q;
        int32_t k = ryu_pow5bits(i) - RYU_DOUBLE_POW5_BITCOUNT;
        int32_t j = (int32_t)q - k;
        vr = ryu_mul_shift_all_64(m2, RYU_DOUBLE_POW5_SPLIT[i], j, &vp, &vm, mm_shift);
        if (q <= 1u) {
            vr_is_trailing_zeros = true;
            if (accept_bounds) {
                vm_is_trailing_zeros = (mm_shift == 1u);
            } else {
                vp--;
            }
        } else if (q < 63u) {
            vr_is_trailing_zeros = ryu_multiple_of_power_of_2(mv, q);
        }
    }

    int32_t removed = 0;
    uint8_t last_removed_digit = 0;
    uint64_t output;

    if (vm_is_trailing_zeros || vr_is_trailing_zeros) {
        for (;;) {
            uint64_t vp_div10 = vp / 10ULL;
            uint64_t vm_div10 = vm / 10ULL;
            if (vp_div10 <= vm_div10) {
                break;
            }
            uint32_t vm_mod10 = (uint32_t)(vm % 10ULL);
            uint64_t vr_div10 = vr / 10ULL;
            uint32_t vr_mod10 = (uint32_t)(vr % 10ULL);
            vm_is_trailing_zeros &= (vm_mod10 == 0u);
            vr_is_trailing_zeros &= (last_removed_digit == 0u);
            last_removed_digit = (uint8_t)vr_mod10;
            vr = vr_div10;
            vp = vp_div10;
            vm = vm_div10;
            removed++;
        }
        if (vm_is_trailing_zeros) {
            for (;;) {
                uint64_t vm_div10 = vm / 10ULL;
                uint32_t vm_mod10 = (uint32_t)(vm % 10ULL);
                if (vm_mod10 != 0u) {
                    break;
                }
                uint64_t vp_div10 = vp / 10ULL;
                uint64_t vr_div10 = vr / 10ULL;
                uint32_t vr_mod10 = (uint32_t)(vr % 10ULL);
                vr_is_trailing_zeros &= (last_removed_digit == 0u);
                last_removed_digit = (uint8_t)vr_mod10;
                vr = vr_div10;
                vp = vp_div10;
                vm = vm_div10;
                removed++;
            }
        }
        if (vr_is_trailing_zeros && last_removed_digit == 5u && (vr % 2ULL) == 0ULL) {
            last_removed_digit = 4u;
        }
        output = vr + ((vr == vm && (!accept_bounds || !vm_is_trailing_zeros)) ||
                               last_removed_digit >= 5u
                           ? 1ULL
                           : 0ULL);
    } else {
        bool round_up = false;
        uint64_t vp_div100 = vp / 100ULL;
        uint64_t vm_div100 = vm / 100ULL;
        if (vp_div100 > vm_div100) {
            uint64_t vr_div100 = vr / 100ULL;
            uint32_t vr_mod100 = (uint32_t)(vr % 100ULL);
            round_up = (vr_mod100 >= 50u);
            vr = vr_div100;
            vp = vp_div100;
            vm = vm_div100;
            removed += 2;
        }
        for (;;) {
            uint64_t vp_div10 = vp / 10ULL;
            uint64_t vm_div10 = vm / 10ULL;
            if (vp_div10 <= vm_div10) {
                break;
            }
            uint64_t vr_div10 = vr / 10ULL;
            uint32_t vr_mod10 = (uint32_t)(vr % 10ULL);
            round_up = (vr_mod10 >= 5u);
            vr = vr_div10;
            vp = vp_div10;
            vm = vm_div10;
            removed++;
        }
        output = vr + ((vr == vm || round_up) ? 1ULL : 0ULL);
    }

    uint32_t olength = ryu_decimal_length17(output);
    int32_t exp10 = e10 + removed + (int32_t)olength - 1;

    char digits[24];
    for (int32_t idx = (int32_t)olength - 1; idx >= 0; idx--) {
        digits[idx] = (char)('0' + (output % 10ULL));
        output /= 10ULL;
    }
    int k = (int)olength;
    while (k > 1 && digits[k - 1] == '0') {
        k--;
    }
    digits[k] = '\0';

    /* Apply ECMAScript Number::toString / RFC 8785 Section 3.2.2.3 decimal-point placement */
    int n = exp10 + 1;
    char tmp[64];
    size_t pos = 0;
    if (sign) {
        tmp[pos++] = '-';
    }

    if (k <= n && n <= 21) {
        memcpy(tmp + pos, digits, (size_t)k);
        pos += (size_t)k;
        memset(tmp + pos, '0', (size_t)(n - k));
        pos += (size_t)(n - k);
    } else if (0 < n && n < k) {
        memcpy(tmp + pos, digits, (size_t)n);
        pos += (size_t)n;
        tmp[pos++] = '.';
        memcpy(tmp + pos, digits + n, (size_t)(k - n));
        pos += (size_t)(k - n);
    } else if (-5 <= n && n <= 0) {
        tmp[pos++] = '0';
        tmp[pos++] = '.';
        memset(tmp + pos, '0', (size_t)(-n));
        pos += (size_t)(-n);
        memcpy(tmp + pos, digits, (size_t)k);
        pos += (size_t)k;
    } else {
        tmp[pos++] = digits[0];
        if (k > 1) {
            tmp[pos++] = '.';
            memcpy(tmp + pos, digits + 1, (size_t)(k - 1));
            pos += (size_t)(k - 1);
        }
        tmp[pos++] = 'e';
        int e_val = n - 1;
        if (e_val >= 0) {
            tmp[pos++] = '+';
        } else {
            tmp[pos++] = '-';
            e_val = -e_val;
        }
        int e_len = 0;
        char ebuf[16];
        do {
            ebuf[e_len++] = (char)('0' + (e_val % 10));
            e_val /= 10;
        } while (e_val > 0);
        for (int ei = e_len - 1; ei >= 0; ei--) {
            tmp[pos++] = ebuf[ei];
        }
    }

    if (pos + 1u > out_cap) {
        return false;
    }
    memcpy(out, tmp, pos);
    out[pos] = '\0';
    return true;
}


typedef struct {
    char *data;
    size_t len;
    size_t cap;
} we_jcs_buf_t;

static bool we_jcs_buf_reserve(we_jcs_buf_t *b, size_t add)
{
    if (add > SIZE_MAX - b->len - 1u) {
        return false;
    }
    size_t need = b->len + add + 1u;
    if (need <= b->cap) {
        return true;
    }
    size_t ncap = b->cap == 0 ? 256u : b->cap;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2u) {
            ncap = need;
            break;
        }
        ncap *= 2u;
    }
    char *nd = realloc(b->data, ncap);
    if (!nd) {
        return false;
    }
    b->data = nd;
    b->cap = ncap;
    return true;
}

static bool we_jcs_append_bytes(we_jcs_buf_t *b, const char *s, size_t len)
{
    if (!we_jcs_buf_reserve(b, len)) {
        return false;
    }
    if (len > 0) {
        memcpy(b->data + b->len, s, len);
        b->len += len;
    }
    return true;
}

static bool we_jcs_append_ch(we_jcs_buf_t *b, char c)
{
    return we_jcs_append_bytes(b, &c, 1u);
}

static bool we_utf8_validate(const char *s)
{
    if (!s) {
        return false;
    }
    const uint8_t *p = (const uint8_t *)s;
    while (*p != 0) {
        uint8_t b0 = p[0];
        if (b0 < 0x80u) {
            p += 1;
        } else if (b0 >= 0xC2u && b0 <= 0xDFu) {
            if ((p[1] & 0xC0u) != 0x80u) {
                return false;
            }
            p += 2;
        } else if (b0 >= 0xE0u && b0 <= 0xEFu) {
            uint8_t b1 = p[1];
            uint8_t b2 = p[2];
            if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) {
                return false;
            }
            if (b0 == 0xE0u && b1 < 0xA0u) {
                return false;
            }
            if (b0 == 0xEDu && b1 > 0x9Fu) {
                return false;
            }
            p += 3;
        } else if (b0 >= 0xF0u && b0 <= 0xF4u) {
            uint8_t b1 = p[1];
            uint8_t b2 = p[2];
            uint8_t b3 = p[3];
            if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u) {
                return false;
            }
            if (b0 == 0xF0u && b1 < 0x90u) {
                return false;
            }
            if (b0 == 0xF4u && b1 > 0x8Fu) {
                return false;
            }
            p += 4;
        } else {
            return false;
        }
    }
    return true;
}

typedef struct {
    const uint8_t *p;
    uint16_t pending_low;
} we_utf16_iter_t;

static int32_t we_utf16_next_unit(we_utf16_iter_t *it)
{
    if (it->pending_low != 0) {
        uint16_t lo = it->pending_low;
        it->pending_low = 0;
        return (int32_t)lo;
    }
    uint8_t b0 = *it->p;
    if (b0 == 0) {
        return -1;
    }
    uint32_t cp;
    if (b0 < 0x80u) {
        cp = b0;
        it->p += 1;
    } else if (b0 < 0xE0u) {
        cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(it->p[1] & 0x3Fu);
        it->p += 2;
    } else if (b0 < 0xF0u) {
        cp = ((uint32_t)(b0 & 0x0Fu) << 12) | ((uint32_t)(it->p[1] & 0x3Fu) << 6) |
             (uint32_t)(it->p[2] & 0x3Fu);
        it->p += 3;
    } else {
        cp = ((uint32_t)(b0 & 0x07u) << 18) | ((uint32_t)(it->p[1] & 0x3Fu) << 12) |
             ((uint32_t)(it->p[2] & 0x3Fu) << 6) | (uint32_t)(it->p[3] & 0x3Fu);
        it->p += 4;
    }
    if (cp < 0x10000u) {
        return (int32_t)cp;
    }
    uint32_t s = cp - 0x10000u;
    it->pending_low = (uint16_t)(0xDC00u | (s & 0x3FFu));
    return (int32_t)(0xD800u | (s >> 10));
}

static int we_jkv_utf16_cmp(const void *pa, const void *pb)
{
    const we_jkv_t *a = *(const we_jkv_t *const *)pa;
    const we_jkv_t *b = *(const we_jkv_t *const *)pb;
    we_utf16_iter_t ia = {.p = (const uint8_t *)a->key, .pending_low = 0};
    we_utf16_iter_t ib = {.p = (const uint8_t *)b->key, .pending_low = 0};
    for (;;) {
        int32_t ua = we_utf16_next_unit(&ia);
        int32_t ub = we_utf16_next_unit(&ib);
        if (ua != ub) {
            return (ua < ub) ? -1 : 1;
        }
        if (ua < 0) {
            return 0;
        }
    }
}

static bool we_jcs_append_string(we_jcs_buf_t *b, const char *s)
{
    if (!we_utf8_validate(s) || !we_jcs_append_ch(b, '"')) {
        return false;
    }
    static const char hex[] = "0123456789abcdef";
    const uint8_t *p = (const uint8_t *)s;
    while (*p != 0) {
        uint8_t c = *p++;
        switch (c) {
        case '"':
            if (!we_jcs_append_bytes(b, "\\\"", 2u)) {
                return false;
            }
            break;
        case '\\':
            if (!we_jcs_append_bytes(b, "\\\\", 2u)) {
                return false;
            }
            break;
        case '\b':
            if (!we_jcs_append_bytes(b, "\\b", 2u)) {
                return false;
            }
            break;
        case '\f':
            if (!we_jcs_append_bytes(b, "\\f", 2u)) {
                return false;
            }
            break;
        case '\n':
            if (!we_jcs_append_bytes(b, "\\n", 2u)) {
                return false;
            }
            break;
        case '\r':
            if (!we_jcs_append_bytes(b, "\\r", 2u)) {
                return false;
            }
            break;
        case '\t':
            if (!we_jcs_append_bytes(b, "\\t", 2u)) {
                return false;
            }
            break;
        default:
            if (c < 0x20u) {
                char esc[6] = {'\\', 'u', '0', '0', hex[(c >> 4) & 0xFu], hex[c & 0xFu]};
                if (!we_jcs_append_bytes(b, esc, 6u)) {
                    return false;
                }
            } else {
                if (!we_jcs_append_ch(b, (char)c)) {
                    return false;
                }
            }
            break;
        }
    }
    return we_jcs_append_ch(b, '"');
}

static bool we_jcs_serialize_val(we_jcs_buf_t *b, const we_jval_t *v, int depth)
{
    if (!v || depth > 64) {
        return false;
    }
    switch (v->type) {
    case JVAL_NULL:
        return we_jcs_append_bytes(b, "null", 4u);
    case JVAL_BOOL:
        return v->bval ? we_jcs_append_bytes(b, "true", 4u)
                       : we_jcs_append_bytes(b, "false", 5u);
    case JVAL_NUM: {
        char nbuf[64];
        if (!we_json_format_double(v->num, nbuf, sizeof(nbuf))) {
            return false;
        }
        return we_jcs_append_bytes(b, nbuf, strlen(nbuf));
    }
    case JVAL_STR:
        return v->str ? we_jcs_append_string(b, v->str) : false;
    case JVAL_ARR:
        if (!we_jcs_append_ch(b, '[')) {
            return false;
        }
        for (size_t i = 0; i < v->arr.count; i++) {
            if (i > 0 && !we_jcs_append_ch(b, ',')) {
                return false;
            }
            if (!we_jcs_serialize_val(b, v->arr.items[i], depth + 1)) {
                return false;
            }
        }
        return we_jcs_append_ch(b, ']');
    case JVAL_OBJ: {
        if (v->obj.count == 0) {
            return we_jcs_append_bytes(b, "{}", 2u);
        }
        const we_jkv_t *stack_sorted[32];
        const we_jkv_t **sorted = stack_sorted;
        if (v->obj.count > 32u) {
            sorted = malloc(v->obj.count * sizeof(*sorted));
            if (!sorted) {
                return false;
            }
        }
        for (size_t i = 0; i < v->obj.count; i++) {
            if (!v->obj.entries[i].key || !v->obj.entries[i].val ||
                !we_utf8_validate(v->obj.entries[i].key)) {
                if (sorted != stack_sorted) {
                    free(sorted);
                }
                return false;
            }
            sorted[i] = &v->obj.entries[i];
        }
        qsort(sorted, v->obj.count, sizeof(*sorted), we_jkv_utf16_cmp);
        for (size_t i = 0; i + 1 < v->obj.count; i++) {
            if (strcmp(sorted[i]->key, sorted[i + 1]->key) == 0) {
                if (sorted != stack_sorted) {
                    free(sorted);
                }
                return false;
            }
        }
        bool ok = we_jcs_append_ch(b, '{');
        for (size_t i = 0; ok && i < v->obj.count; i++) {
            if (i > 0) {
                ok = we_jcs_append_ch(b, ',');
            }
            if (ok) {
                ok = we_jcs_append_string(b, sorted[i]->key);
            }
            if (ok) {
                ok = we_jcs_append_ch(b, ':');
            }
            if (ok) {
                ok = we_jcs_serialize_val(b, sorted[i]->val, depth + 1);
            }
        }
        if (sorted != stack_sorted) {
            free(sorted);
        }
        return ok && we_jcs_append_ch(b, '}');
    }
    }
    return false;
}

char *we_jval_to_canonical_json(const we_jval_t *v, size_t *out_len)
{
    if (out_len) {
        *out_len = 0;
    }
    if (!v) {
        return NULL;
    }
    we_jcs_buf_t b = {0};
    if (!we_jcs_serialize_val(&b, v, 0)) {
        free(b.data);
        return NULL;
    }
    b.data[b.len] = '\0';
    if (out_len) {
        *out_len = b.len;
    }
    return b.data;
}

void we_jval_free(we_jval_t *v)
{
    if (!v) {
        return;
    }
    if (v->type == JVAL_STR) {
        free(v->str);
    } else if (v->type == JVAL_ARR) {
        for (size_t i = 0; i < v->arr.count; i++) {
            we_jval_free(v->arr.items[i]);
        }
        free(v->arr.items);
    } else if (v->type == JVAL_OBJ) {
        for (size_t i = 0; i < v->obj.count; i++) {
            free(v->obj.entries[i].key);
            we_jval_free(v->obj.entries[i].val);
        }
        free(v->obj.entries);
    }
    free(v);
}

static void json_skip_ws(json_lexer_t *lx)
{
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            lx->pos++;
        } else if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '/') {
            lx->pos += 2;
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n') {
                lx->pos++;
            }
        } else if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '*') {
            lx->pos += 2;
            while (lx->pos + 1 < lx->len &&
                   !(lx->src[lx->pos] == '*' && lx->src[lx->pos + 1] == '/')) {
                lx->pos++;
            }
            if (lx->pos + 1 < lx->len) {
                lx->pos += 2;
            } else {
                lx->pos = lx->len;
            }
        } else {
            break;
        }
    }
}

static inline bool is_json_delim(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ']' || c == '}' ||
           c == ':' || c == '/';
}

static char *json_parse_raw_string(json_lexer_t *lx)
{
    json_skip_ws(lx);
    if (lx->pos >= lx->len || lx->src[lx->pos] != '"') {
        return NULL;
    }
    lx->pos++;

    /* Fast path: scan for closing quote when no escape backslash or control char exists */
    const char *start = lx->src + lx->pos;
    size_t rem = lx->len - lx->pos;
    const char *quote = memchr(start, '"', rem);
    if (quote) {
        size_t slen = (size_t)(quote - start);
        const char *esc = memchr(start, '\\', slen);
        if (!esc) {
            for (size_t i = 0; i < slen; i++) {
                if ((unsigned char)start[i] < 0x20u) {
                    return NULL;
                }
            }
            char *out = malloc(slen + 1);
            if (!out) {
                return NULL;
            }
            memcpy(out, start, slen);
            out[slen] = '\0';
            lx->pos += slen + 1;
            return out;
        }
    }

    size_t cap = 64;
    size_t len = 0;
    char *out = malloc(cap);
    if (!out) {
        return NULL;
    }

    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos++];
        if (c == '"') {
            out[len] = '\0';
            return out;
        }
        if ((unsigned char)c < 0x20u) {
            free(out);
            return NULL;
        }
        if (c == '\\') {
            if (lx->pos >= lx->len) {
                free(out);
                return NULL;
            }
            char esc = lx->src[lx->pos++];
            switch (esc) {
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case '/':
                c = '/';
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'u': {
                if (lx->pos + 4 > lx->len || !isxdigit((unsigned char)lx->src[lx->pos]) ||
                    !isxdigit((unsigned char)lx->src[lx->pos + 1]) ||
                    !isxdigit((unsigned char)lx->src[lx->pos + 2]) ||
                    !isxdigit((unsigned char)lx->src[lx->pos + 3])) {
                    free(out);
                    return NULL;
                }
                char hex[5];
                memcpy(hex, lx->src + lx->pos, 4);
                hex[4] = '\0';
                uint32_t cp = (uint32_t)strtoul(hex, NULL, 16);
                lx->pos += 4;
                if (cp >= 0xD800u && cp <= 0xDBFFu && lx->pos + 6 <= lx->len &&
                    lx->src[lx->pos] == '\\' && lx->src[lx->pos + 1] == 'u' &&
                    isxdigit((unsigned char)lx->src[lx->pos + 2]) &&
                    isxdigit((unsigned char)lx->src[lx->pos + 3]) &&
                    isxdigit((unsigned char)lx->src[lx->pos + 4]) &&
                    isxdigit((unsigned char)lx->src[lx->pos + 5])) {
                    char hex2[5];
                    memcpy(hex2, lx->src + lx->pos + 2, 4);
                    hex2[4] = '\0';
                    uint32_t low = (uint32_t)strtoul(hex2, NULL, 16);
                    if (low >= 0xDC00u && low <= 0xDFFFu) {
                        cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                        lx->pos += 6;
                    }
                }
                /* Unpaired surrogates map to U+FFFD for existing WE fixture compatibility. */
                if (cp >= 0xD800u && cp <= 0xDFFFu) {
                    cp = 0xFFFDu;
                }
                /* Reject \u0000 so embedded NUL never truncates or mutates C string keys/values. */
                if (cp == 0u || cp > 0x10FFFFu) {
                    free(out);
                    return NULL;
                }
                if (len + 6 > cap) {
                    size_t ncap = cap * 2;
                    char *nb = realloc(out, ncap);
                    if (!nb) {
                        free(out);
                        return NULL;
                    }
                    out = nb;
                    cap = ncap;
                }
                if (cp < 0x80u) {
                    out[len++] = (char)cp;
                } else if (cp < 0x800u) {
                    out[len++] = (char)(0xC0u | (cp >> 6));
                    out[len++] = (char)(0x80u | (cp & 0x3Fu));
                } else if (cp < 0x10000u) {
                    out[len++] = (char)(0xE0u | (cp >> 12));
                    out[len++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                    out[len++] = (char)(0x80u | (cp & 0x3Fu));
                } else {
                    out[len++] = (char)(0xF0u | (cp >> 18));
                    out[len++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
                    out[len++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                    out[len++] = (char)(0x80u | (cp & 0x3Fu));
                }
                continue;
            }
            default:
                free(out);
                return NULL;
            }
        }
        if (len + 2 > cap) {
            size_t ncap = cap * 2;
            char *nb = realloc(out, ncap);
            if (!nb) {
                free(out);
                return NULL;
            }
            out = nb;
            cap = ncap;
        }
        out[len++] = c;
    }

    free(out);
    return NULL;
}

static we_jval_t *json_parse_value(json_lexer_t *lx, int depth);

static we_jval_t *json_parse_array(json_lexer_t *lx, int depth)
{
    lx->pos++;
    we_jval_t *arr = calloc(1, sizeof(we_jval_t));
    if (!arr) {
        return NULL;
    }
    arr->type = JVAL_ARR;

    json_skip_ws(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == ']') {
        lx->pos++;
        return arr;
    }

    size_t cap = 0;
    while (lx->pos < lx->len) {
        we_jval_t *elem = json_parse_value(lx, depth + 1);
        if (!elem) {
            we_jval_free(arr);
            return NULL;
        }
        if (arr->arr.count + 1 > cap) {
            size_t ncap = cap == 0 ? 8 : cap * 2;
            we_jval_t **ni = realloc(arr->arr.items, ncap * sizeof(we_jval_t *));
            if (!ni) {
                we_jval_free(elem);
                we_jval_free(arr);
                return NULL;
            }
            arr->arr.items = ni;
            cap = ncap;
        }
        arr->arr.items[arr->arr.count++] = elem;

        json_skip_ws(lx);
        if (lx->pos >= lx->len) {
            we_jval_free(arr);
            return NULL;
        }
        if (lx->src[lx->pos] == ']') {
            lx->pos++;
            return arr;
        }
        if (lx->src[lx->pos] != ',') {
            we_jval_free(arr);
            return NULL;
        }
        lx->pos++;
    }

    we_jval_free(arr);
    return NULL;
}

static we_jval_t *json_parse_object(json_lexer_t *lx, int depth)
{
    lx->pos++;
    we_jval_t *obj = calloc(1, sizeof(we_jval_t));
    if (!obj) {
        return NULL;
    }
    obj->type = JVAL_OBJ;

    json_skip_ws(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == '}') {
        lx->pos++;
        return obj;
    }

    size_t cap = 0;
    while (lx->pos < lx->len) {
        char *key = json_parse_raw_string(lx);
        if (!key) {
            we_jval_free(obj);
            return NULL;
        }
        json_skip_ws(lx);
        if (lx->pos >= lx->len || lx->src[lx->pos] != ':') {
            free(key);
            we_jval_free(obj);
            return NULL;
        }
        lx->pos++;

        we_jval_t *val = json_parse_value(lx, depth + 1);
        if (!val) {
            free(key);
            we_jval_free(obj);
            return NULL;
        }

        if (obj->obj.count + 1 > cap) {
            size_t ncap = cap == 0 ? 8 : cap * 2;
            we_jkv_t *ne = realloc(obj->obj.entries, ncap * sizeof(we_jkv_t));
            if (!ne) {
                free(key);
                we_jval_free(val);
                we_jval_free(obj);
                return NULL;
            }
            obj->obj.entries = ne;
            cap = ncap;
        }
        obj->obj.entries[obj->obj.count].key = key;
        obj->obj.entries[obj->obj.count].val = val;
        obj->obj.count++;

        json_skip_ws(lx);
        if (lx->pos >= lx->len) {
            we_jval_free(obj);
            return NULL;
        }
        if (lx->src[lx->pos] == '}') {
            lx->pos++;
            return obj;
        }
        if (lx->src[lx->pos] != ',') {
            we_jval_free(obj);
            return NULL;
        }
        lx->pos++;
    }

    we_jval_free(obj);
    return NULL;
}

static we_jval_t *json_parse_number(json_lexer_t *lx)
{
    size_t start = lx->pos;
    size_t p = start;
    if (lx->src[p] == '-') {
        p++;
    }
    if (p >= lx->len) {
        return NULL;
    }
    if (lx->src[p] == '0') {
        p++;
        if (p < lx->len && lx->src[p] >= '0' && lx->src[p] <= '9') {
            return NULL;
        }
    } else if (lx->src[p] >= '1' && lx->src[p] <= '9') {
        while (p < lx->len && lx->src[p] >= '0' && lx->src[p] <= '9') {
            p++;
        }
    } else {
        return NULL;
    }

    if (p < lx->len && lx->src[p] == '.') {
        p++;
        if (p >= lx->len || lx->src[p] < '0' || lx->src[p] > '9') {
            return NULL;
        }
        while (p < lx->len && lx->src[p] >= '0' && lx->src[p] <= '9') {
            p++;
        }
    }

    if (p < lx->len && (lx->src[p] == 'e' || lx->src[p] == 'E')) {
        p++;
        if (p < lx->len && (lx->src[p] == '+' || lx->src[p] == '-')) {
            p++;
        }
        if (p >= lx->len || lx->src[p] < '0' || lx->src[p] > '9') {
            return NULL;
        }
        while (p < lx->len && lx->src[p] >= '0' && lx->src[p] <= '9') {
            p++;
        }
    }

    if (p < lx->len && !is_json_delim(lx->src[p])) {
        return NULL;
    }

    size_t nlen = p - start;
    char stack_buf[128];
    char *tok = stack_buf;
    if (nlen >= sizeof(stack_buf)) {
        tok = malloc(nlen + 1);
        if (!tok) {
            return NULL;
        }
    }
    memcpy(tok, lx->src + start, nlen);
    tok[nlen] = '\0';

    errno = 0;
    char *endptr = NULL;
    double num = we_strtod_c(tok, &endptr);
    bool ok = (endptr == tok + nlen && isfinite(num));
    if (tok != stack_buf) {
        free(tok);
    }
    if (!ok) {
        return NULL;
    }

    lx->pos = p;
    we_jval_t *v = calloc(1, sizeof(we_jval_t));
    if (v) {
        v->type = JVAL_NUM;
        v->num = num;
    }
    return v;
}

static we_jval_t *json_parse_value(json_lexer_t *lx, int depth)
{
    if (depth > 64) {
        return NULL;
    }
    json_skip_ws(lx);
    if (lx->pos >= lx->len) {
        return NULL;
    }

    char c = lx->src[lx->pos];
    if (c == '{') {
        return json_parse_object(lx, depth);
    }
    if (c == '[') {
        return json_parse_array(lx, depth);
    }
    if (c == '"') {
        char *s = json_parse_raw_string(lx);
        if (!s) {
            return NULL;
        }
        we_jval_t *v = calloc(1, sizeof(we_jval_t));
        if (!v) {
            free(s);
            return NULL;
        }
        v->type = JVAL_STR;
        v->str = s;
        return v;
    }
    if (lx->len - lx->pos >= 4 && memcmp(lx->src + lx->pos, "true", 4) == 0 &&
        (lx->pos + 4 == lx->len || is_json_delim(lx->src[lx->pos + 4]))) {
        lx->pos += 4;
        we_jval_t *v = calloc(1, sizeof(we_jval_t));
        if (v) {
            v->type = JVAL_BOOL;
            v->bval = true;
        }
        return v;
    }
    if (lx->len - lx->pos >= 5 && memcmp(lx->src + lx->pos, "false", 5) == 0 &&
        (lx->pos + 5 == lx->len || is_json_delim(lx->src[lx->pos + 5]))) {
        lx->pos += 5;
        we_jval_t *v = calloc(1, sizeof(we_jval_t));
        if (v) {
            v->type = JVAL_BOOL;
            v->bval = false;
        }
        return v;
    }
    if (lx->len - lx->pos >= 4 && memcmp(lx->src + lx->pos, "null", 4) == 0 &&
        (lx->pos + 4 == lx->len || is_json_delim(lx->src[lx->pos + 4]))) {
        lx->pos += 4;
        return calloc(1, sizeof(we_jval_t));
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        return json_parse_number(lx);
    }
    return NULL;
}

we_jval_t *we_json_parse_root(const char *src, size_t len)
{
    if (!src || len == 0) {
        return NULL;
    }
    size_t offset = 0;
    if (len >= 3 && (uint8_t)src[0] == 0xEF && (uint8_t)src[1] == 0xBB && (uint8_t)src[2] == 0xBF) {
        offset = 3;
    }
    json_lexer_t lx = {.src = src + offset, .len = len - offset, .pos = 0};
    we_jval_t *root = json_parse_value(&lx, 0);
    if (!root) {
        return NULL;
    }
    json_skip_ws(&lx);
    if (lx.pos != lx.len) {
        we_jval_free(root);
        return NULL;
    }
    return root;
}

const we_jval_t *we_jobj_get(const we_jval_t *obj, const char *key)
{
    if (!obj || obj->type != JVAL_OBJ || !key) {
        return NULL;
    }
    char k0 = key[0];
    for (size_t i = 0; i < obj->obj.count; i++) {
        if (obj->obj.entries[i].key[0] == k0 && strcmp(obj->obj.entries[i].key, key) == 0) {
            return obj->obj.entries[i].val;
        }
    }
    char k0_lower = (char)tolower((unsigned char)k0);
    for (size_t i = 0; i < obj->obj.count; i++) {
        if ((char)tolower((unsigned char)obj->obj.entries[i].key[0]) == k0_lower &&
            strcasecmp(obj->obj.entries[i].key, key) == 0) {
            return obj->obj.entries[i].val;
        }
    }
    return NULL;
}

const char *we_jval_str(const we_jval_t *v, const char *def)
{
    if (!v) {
        return def;
    }
    if (v->type == JVAL_STR && v->str) {
        return v->str;
    }
    if (v->type == JVAL_OBJ) {
        const we_jval_t *inner = we_jobj_get(v, "value");
        if (inner && inner->type == JVAL_STR && inner->str) {
            return inner->str;
        }
    }
    return def;
}

const we_user_prop_t *we_project_find_prop(const we_project_t *proj, const char *key)
{
    if (!proj || !key || key[0] == '\0') {
        return NULL;
    }
    char k0_lower = (char)tolower((unsigned char)key[0]);
    for (uint32_t i = 0; i < proj->prop_count; i++) {
        if ((char)tolower((unsigned char)proj->props[i].key[0]) == k0_lower &&
            strcasecmp(proj->props[i].key, key) == 0) {
            return &proj->props[i];
        }
    }
    return NULL;
}

static bool eval_up_matches_cond_str(const we_user_prop_t *up, const char *cond_str)
{
    if (!up || !cond_str) {
        return false;
    }
    bool invert = false;
    while (*cond_str == '!') {
        invert = !invert;
        cond_str++;
    }
    while (*cond_str && isspace((unsigned char)*cond_str)) {
        cond_str++;
    }

    bool matched = false;
    if (up->str_val[0] != '\0' && strcasecmp(up->str_val, cond_str) == 0) {
        matched = true;
    } else if (up->num_components > 0) {
        if (strcasecmp(cond_str, "true") == 0) {
            matched = (up->val[0] != 0.0f);
        } else if (strcasecmp(cond_str, "false") == 0) {
            matched = (up->val[0] == 0.0f);
        } else {
            char *endp = NULL;
            float cv = we_strtof_c(cond_str, &endp);
            if (endp && endp > cond_str && isfinite(cv)) {
                while (*endp && isspace((unsigned char)*endp)) {
                    endp++;
                }
                if (*endp == '\0') {
                    matched = (fabsf(up->val[0] - cv) < 1e-4f);
                }
            }
        }
    }
    return invert ? !matched : matched;
}

bool we_project_eval_prop_bool(const we_project_t *proj, const char *prop_expr, bool *out_val)
{
    if (!proj || !prop_expr || prop_expr[0] == '\0' || !out_val) {
        return false;
    }
    const char *p = prop_expr;
    bool prefix_neg = false;
    while (*p == '!' || isspace((unsigned char)*p)) {
        if (*p == '!') {
            prefix_neg = !prefix_neg;
        }
        p++;
    }
    if (*p == '\0') {
        return false;
    }
    const char *eq = strchr(p, '=');
    if (!eq) {
        const we_user_prop_t *up = we_project_find_prop(proj, p);
        if (!up || up->num_components == 0) {
            return false;
        }
        bool v = (up->val[0] != 0.0f);
        *out_val = prefix_neg ? !v : v;
        return true;
    }

    size_t klen = (size_t)(eq - p);
    bool neq = prefix_neg;
    if (klen > 0 && p[klen - 1] == '!') {
        neq = !neq;
        klen--;
    }
    while (klen > 0 && isspace((unsigned char)p[klen - 1])) {
        klen--;
    }
    if (klen == 0 || klen >= 64) {
        return false;
    }
    char kbuf[64];
    memcpy(kbuf, p, klen);
    kbuf[klen] = '\0';

    const we_user_prop_t *up = we_project_find_prop(proj, kbuf);
    if (!up) {
        return false;
    }
    bool matched = eval_up_matches_cond_str(up, eq + 1);
    *out_val = neq ? !matched : matched;
    return true;
}

uint32_t we_parse_float_str(const char *s, float *out, uint32_t max_n)
{
    if (!s || !out || max_n == 0) {
        return 0;
    }
    const char *p = s;
    uint32_t count = 0;
    while (*p && count < max_n) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) {
            p++;
        }
        if (!*p) {
            break;
        }
        char *endptr = NULL;
        float v = we_strtof_c(p, &endptr);
        if (endptr == p || !isfinite(v)) {
            break;
        }
        out[count++] = v;
        p = endptr;
    }
    return count;
}

uint32_t we_jval_floats(const we_jval_t *v, const we_project_t *proj, float *out, uint32_t max_n,
                        char *out_user_prop, size_t prop_cap)
{
    if (out_user_prop && prop_cap > 0) {
        out_user_prop[0] = '\0';
    }
    if (!v || !out || max_n == 0) {
        return 0;
    }

    if (v->type == JVAL_OBJ) {
        const we_jval_t *u = we_jobj_get(v, "user");
        const char *ukey = NULL;
        const we_jval_t *ucond = NULL;
        if (u && u->type == JVAL_STR) {
            ukey = u->str;
        } else if (u && u->type == JVAL_OBJ) {
            ukey = we_jval_str(we_jobj_get(u, "name"), NULL);
            ucond = we_jobj_get(u, "condition");
        }
        if (ukey && ukey[0] != '\0') {
            bool key_neg = false;
            const char *clean_key = ukey;
            while (*clean_key == '!' || isspace((unsigned char)*clean_key)) {
                if (*clean_key == '!') {
                    key_neg = !key_neg;
                }
                clean_key++;
            }
            char cond_str[64] = {0};
            if (ucond) {
                if (ucond->type == JVAL_STR && ucond->str) {
                    snprintf(cond_str, sizeof(cond_str), "%s", ucond->str);
                } else if (ucond->type == JVAL_NUM) {
                    if (!we_json_format_double(ucond->num, cond_str, sizeof(cond_str))) {
                        cond_str[0] = '\0';
                    }
                } else if (ucond->type == JVAL_BOOL) {
                    snprintf(cond_str, sizeof(cond_str), "%s", ucond->bval ? "true" : "false");
                }
            }

            if (out_user_prop && prop_cap > 0) {
                if (cond_str[0] != '\0') {
                    bool cond_neg = key_neg;
                    const char *cp = cond_str;
                    while (*cp == '!' || isspace((unsigned char)*cp)) {
                        if (*cp == '!') {
                            cond_neg = !cond_neg;
                        }
                        cp++;
                    }
                    snprintf(out_user_prop, prop_cap, "%s%s%s", clean_key, cond_neg ? "!=" : "=",
                             cp);
                } else {
                    snprintf(out_user_prop, prop_cap, "%s%s", key_neg ? "!" : "", clean_key);
                }
            }

            const we_user_prop_t *up = we_project_find_prop(proj, clean_key);
            if (up) {
                if (cond_str[0] != '\0') {
                    bool matched = eval_up_matches_cond_str(up, cond_str);
                    if (key_neg) {
                        matched = !matched;
                    }
                    out[0] = matched ? 1.0f : 0.0f;
                    return 1;
                }
                if (up->num_components > 0) {
                    uint32_t n = up->num_components < max_n ? up->num_components : max_n;
                    for (uint32_t i = 0; i < n; i++) {
                        out[i] = key_neg ? (up->val[i] == 0.0f ? 1.0f : 0.0f) : up->val[i];
                    }
                    return n;
                }
            }
        }
        const we_jval_t *inner = we_jobj_get(v, "value");
        if (inner) {
            /* Preserve an outer unresolved user-prop name rather than wiping caller out_user_prop. */
            if (out_user_prop && prop_cap > 0 && out_user_prop[0] == '\0') {
                return we_jval_floats(inner, proj, out, max_n, out_user_prop, prop_cap);
            }
            return we_jval_floats(inner, proj, out, max_n, NULL, 0);
        }
        return 0;
    }

    if (v->type == JVAL_NUM) {
        float fv = (float)v->num;
        if (!isfinite(fv)) {
            return 0;
        }
        out[0] = fv;
        return 1;
    }
    if (v->type == JVAL_BOOL) {
        out[0] = v->bval ? 1.0f : 0.0f;
        return 1;
    }
    if (v->type == JVAL_STR && v->str) {
        if (strcasecmp(v->str, "true") == 0) {
            out[0] = 1.0f;
            return 1;
        }
        if (strcasecmp(v->str, "false") == 0) {
            out[0] = 0.0f;
            return 1;
        }
        return we_parse_float_str(v->str, out, max_n);
    }
    if (v->type == JVAL_ARR) {
        uint32_t n = (uint32_t)v->arr.count < max_n ? (uint32_t)v->arr.count : max_n;
        for (uint32_t i = 0; i < n; i++) {
            float tmp = 0.0f;
            if (we_jval_floats(v->arr.items[i], proj, &tmp, 1, NULL, 0) == 0) {
                return i;
            }
            out[i] = tmp;
        }
        return n;
    }
    return 0;
}

float we_jval_float(const we_jval_t *v, const we_project_t *proj, float def, char *out_user_prop,
                    size_t prop_cap)
{
    float tmp = def;
    if (we_jval_floats(v, proj, &tmp, 1, out_user_prop, prop_cap) >= 1) {
        return tmp;
    }
    return def;
}

bool we_jval_bool_resolved(const we_jval_t *v, const we_project_t *proj, bool def,
                           char *out_user_prop, size_t prop_cap)
{
    float tmp = def ? 1.0f : 0.0f;
    if (we_jval_floats(v, proj, &tmp, 1, out_user_prop, prop_cap) >= 1) {
        return tmp != 0.0f;
    }
    return def;
}

we_jval_t *we_json_parse(const char *src, size_t len) { return we_json_parse_root(src, len); }
