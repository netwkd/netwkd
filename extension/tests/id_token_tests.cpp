#include "support/fault_log.h"
#include "support/netw_cells.h"
#include "support/netw_test.h"
#include "support/test_rsa_keys.h"

#include "godot/crypto.hpp"
#include "godot/json.hpp"
#include "godot/local_vector.hpp"
#include "godot/time.hpp"
#include "godot/utility.hpp"
#include "netw/api/id_token.hpp"
#include "netw/api/join_request.hpp"
#include "netw/api/promise.hpp"
#include "netw/id_token.hpp"

#include <initializer_list>
#include <utility>
#include <vector>

namespace TestNetwIdToken {

using namespace godot;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;

namespace id_token = netw::id_token;

constexpr const char *CHALLENGE = "0123456789abcdef0123456789abcdef";
constexpr const char *APP = "1561065725962092666";
constexpr const char *SUBJECT = "123456789012345678";
constexpr int64_t NOW = 1800000000;

Ref<CryptoKey> private_key(const char *p_pem) {
    Ref<CryptoKey> key = netw::gd::new_crypto_key();
    key->load_from_string(String(p_pem), false);
    return key;
}

Ref<CryptoKey> public_key(const Ref<CryptoKey> &p_private) {
    Ref<CryptoKey> key = netw::gd::new_crypto_key();
    key->load_from_string(p_private->save_to_string(true), true);
    return key;
}

Dictionary with(std::initializer_list<std::pair<const char *, Variant>> p) {
    Dictionary made;
    for (const std::pair<const char *, Variant> &field : p) {
        made[String(field.first)] = field.second;
    }
    return made;
}

Array listing(std::initializer_list<Variant> p_items) {
    Array made;
    for (const Variant &item : p_items) {
        made.push_back(item);
    }
    return made;
}

Dictionary merged(const Dictionary &p_base, const Dictionary &p_changes) {
    Dictionary made = p_base.duplicate();
    const Array keys = p_changes.keys();
    for (int64_t at = 0; at < keys.size(); ++at) {
        made[keys[at]] = p_changes[keys[at]];
    }
    return made;
}

String encoded_json(const Variant &p_value) {
    return id_token::encode_base64url(
        JSON::stringify(p_value).to_utf8_buffer()
    );
}

String signed_token(
    const Dictionary &p_header,
    const Dictionary &p_claims,
    const Ref<CryptoKey> &p_signer
) {
    const String part
        = encoded_json(p_header) + String(".") + encoded_json(p_claims);
    return part + String(".")
        + id_token::encode_base64url(
               netw::gd::rs256_sign(part.sha256_buffer(), p_signer)
        );
}

Array proof_of(
    const String &p_token,
    const Ref<CryptoKey> &p_proof_key,
    const String &p_signed
) {
    return listing({
        p_token,
        p_proof_key->save_to_string(true),
        netw::gd::rs256_sign(p_signed.sha256_buffer(), p_proof_key),
    });
}

PackedByteArray modulus_of(const Ref<CryptoKey> &p_key) {
    const PackedStringArray lines
        = p_key->save_to_string(true).strip_edges().split("\n");
    String body;
    for (int64_t at = 1; at + 1 < lines.size(); ++at) {
        body += lines[at];
    }
    body = body.replace("+", "-").replace("/", "_").replace("=", "");
    PackedByteArray der;
    id_token::decode_base64url(body, der);
    return der.slice(33, 33 + 256);
}

Dictionary jwk_of(const char *p_kid, const PackedByteArray &p_modulus) {
    return with({
        {"kid", String(p_kid)},
        {"kty", String("RSA")},
        {"alg", String("RS256")},
        {"use", String("sig")},
        {"n", id_token::encode_base64url(p_modulus)},
        {"e", String("AQAB")},
    });
}

PackedByteArray jwks_of(const Array &p_keys) {
    return JSON::stringify(with({{"keys", p_keys}})).to_utf8_buffer();
}

Ref<netw::NetwJoinRequest> request_for(const String &p_challenge) {
    Ref<netw::NetwJoinRequest> request;
    request.instantiate();
    request->bind_to(9, StringName("ana"), Array(), p_challenge, true);
    return request;
}

bool answered_ok(const Variant &p_answer) {
    return p_answer.get_type() == Variant::NIL;
}

int64_t ticks() {
    return int64_t(Time::get_singleton()->get_ticks_msec());
}

Ref<netw::NetwPromise> promise_in(const Variant &p_value) {
    return Ref<netw::NetwPromise>(
        Object::cast_to<netw::NetwPromise>(netw::gd::live_object(p_value))
    );
}

struct Keys {
    String label;
    Ref<CryptoKey> issuer;
    Ref<CryptoKey> other;
    Ref<CryptoKey> proof;
};

typedef netw_test::LawRowFor<Keys> Row;

Dictionary discord_claims(const Keys &p_keys) {
    return with({
        {"iss", String("https://discord.com")},
        {"aud", String(APP)},
        {"nonce", p_keys.proof->save_to_string(true).sha256_text()},
        {"sub", String(SUBJECT)},
        {"iat", int64_t(1)},
        {"exp", int64_t(4102444800)},
    });
}

Ref<netw::NetwIdToken> discord_server(const Keys &p_keys) {
    Ref<netw::NetwIdToken> token;
    token.instantiate();
    token->set_issuer("https://discord.com");
    token->set_audience(APP);
    token->set_prefix("discord");
    token->keys_answered(
        id_token::KEYS_RESULT_SUCCESS,
        200,
        jwks_of(listing({jwk_of("fixture-rsa", modulus_of(p_keys.issuer))})),
        0
    );
    return token;
}

enum Twist {
    PLAIN,
    OTHER_SIGNER,
    TAMPERED,
    MALFORMED_JSON,
    RAW,
    ANOTHER_CHALLENGE,
    NOT_AN_ARRAY,
    DROP_EXP,
};

struct Variation {
    const char *label;
    Dictionary header;
    Dictionary claims;
    Twist twist;
    const char *raw;
};

Variant proof_for(const Keys &p_keys, const Variation &p_variation) {
    const Dictionary header = with({
        {"alg", String("RS256")},
        {"kid", String("fixture-rsa")},
    });
    const Dictionary claims = discord_claims(p_keys);
    Dictionary row_claims = merged(claims, p_variation.claims);
    if (p_variation.twist == DROP_EXP) {
        row_claims.erase(String("exp"));
    }
    const String made = signed_token(
        merged(header, p_variation.header),
        row_claims,
        p_variation.twist == OTHER_SIGNER ? p_keys.other : p_keys.issuer
    );
    const PackedStringArray parts = made.split(".");
    String text = made;
    if (p_variation.twist == TAMPERED) {
        text = parts[0] + String(".")
            + encoded_json(merged(claims, with({{"sub", String("999")}})))
            + String(".") + parts[2];
    } else if (p_variation.twist == MALFORMED_JSON) {
        text = id_token::encode_base64url(String("not json").to_utf8_buffer())
            + String(".") + parts[1] + String(".") + parts[2];
    } else if (p_variation.twist == RAW) {
        text = String(p_variation.raw);
    }
    const String signed_challenge = String(
        p_variation.twist == ANOTHER_CHALLENGE ? "another" : CHALLENGE
    );
    const Array proof = proof_of(text, p_keys.proof, signed_challenge);
    if (p_variation.twist == NOT_AN_ARRAY) {
        return netw::gd::var_to_bytes(proof);
    }
    return proof;
}

LawVerdict holding_token(const Keys &p_keys) {
    const Dictionary none;
    const Variation variations[] = {
        {"valid", none, none, PLAIN, ""},
        {"name", none, with({{"name", String("Ann")}}), PLAIN, ""},
        {"audience listed alone",
         none,
         with({{"aud", listing({String(APP)})}}),
         PLAIN,
         ""},
        {"issued inside skew", none, with({{"iat", NOW + 30}}), PLAIN, ""},
        {"not before inside skew", none, with({{"nbf", NOW + 30}}), PLAIN, ""},
        {"expired inside skew",
         none,
         with({{"iat", NOW - 600}, {"exp", NOW - 30}}),
         PLAIN,
         ""},
    };
    const Ref<netw::NetwIdToken> server = discord_server(p_keys);
    for (const Variation &variation : variations) {
        const Ref<netw::NetwJoinRequest> request
            = request_for(String(CHALLENGE));
        const Variant answer = server->verify_at(
            request,
            proof_for(p_keys, variation),
            NOW,
            1
        );
        const PackedStringArray proven = request->get_accounts();
        if (!answered_ok(answer) || proven.size() != 1) {
            return law_broken("%s proved nothing", variation.label);
        }
        if (proven[0] != String("discord:") + SUBJECT) {
            return law_broken("%s proved another account", variation.label);
        }
        const String name = variation.claims.get(String("name"), String());
        if (request->name_of(proven[0]) != name) {
            return law_broken("%s carried another name", variation.label);
        }
    }
    return law_held();
}

LawVerdict forged_or_stale_token(const Keys &p_keys) {
    const Dictionary none;
    const Variation variations[] = {
        {"nonce", none, with({{"nonce", String("another-host")}}), PLAIN, ""},
        {"audience", none, with({{"aud", String("another")}}), PLAIN, ""},
        {"audience type",
         none,
         with({{"aud", int64_t(1561065725962092666)}}),
         PLAIN,
         ""},
        {"audience listed with another",
         none,
         with(
             {{"aud", listing({String(APP), String("another")})},
              {"azp", String(APP)}}
         ),
         PLAIN,
         ""},
        {"authorized party",
         none,
         with({{"azp", String("another")}}),
         PLAIN,
         ""},
        {"issuer",
         none,
         with({{"iss", String("https://discord.com/")}}),
         PLAIN,
         ""},
        {"subject empty", none, with({{"sub", String("")}}), PLAIN, ""},
        {"name not text", none, with({{"name", int64_t(5)}}), PLAIN, ""},
        {"missing exp", none, none, DROP_EXP, ""},
        {"algorithm", with({{"alg", String("HS256")}}), none, PLAIN, ""},
        {"unsigned", with({{"alg", String("none")}}), none, PLAIN, ""},
        {"unknown key", with({{"kid", String("rotated")}}), none, PLAIN, ""},
        {"key id not text", with({{"kid", int64_t(7)}}), none, PLAIN, ""},
        {"critical",
         with({{"crit", listing({String("extension")})}}),
         none,
         PLAIN,
         ""},
        {"unencoded payload", with({{"b64", false}}), none, PLAIN, ""},
        {"signature", none, none, OTHER_SIGNER, ""},
        {"tampered", none, none, TAMPERED, ""},
        {"malformed json", none, none, MALFORMED_JSON, ""},
        {"issued past skew", none, with({{"iat", NOW + 120}}), PLAIN, ""},
        {"not before past skew", none, with({{"nbf", NOW + 120}}), PLAIN, ""},
        {"expired past skew",
         none,
         with({{"iat", NOW - 600}, {"exp", NOW - 120}}),
         PLAIN,
         ""},
        {"empty", none, none, RAW, ""},
        {"two parts", none, none, RAW, "a.b"},
        {"four parts", none, none, RAW, "a.b.c.d"},
        {"another challenge", none, none, ANOTHER_CHALLENGE, ""},
        {"proof not an array", none, none, NOT_AN_ARRAY, ""},
    };
    const Ref<netw::NetwIdToken> server = discord_server(p_keys);
    for (const Variation &variation : variations) {
        const Ref<netw::NetwJoinRequest> request
            = request_for(String(CHALLENGE));
        const Variant answer = server->verify_at(
            request,
            proof_for(p_keys, variation),
            NOW,
            1
        );
        if (!answered_ok(answer)) {
            return law_broken("%s refused the whole join", variation.label);
        }
        if (request->get_accounts().size() != 0) {
            return law_broken("%s proved an account", variation.label);
        }
    }
    return law_held();
}

LawVerdict fixed_key(const Keys &p_keys) {
    struct Shape {
        const char *iss;
        const char *aud;
        const char *prefix;
        const char *sub;
        const char *name;
        const char *instance;
    };
    const Shape nakama = {
        "netw-nakama",
        "my-game",
        "nakama",
        "2ca95571-c4b7-4258-8ff2-f49734f40b38",
        "ana",
        "",
    };
    const Shape worker = {
        "https://1561065725962092666.discordsays.com",
        APP,
        "discord",
        SUBJECT,
        "Ana",
        "i-1520000000000000000-gc-1-2",
    };
    struct Arm {
        const char *label;
        const Shape *shape;
        bool fixed_key;
        const char *kid;
        bool signed_by_issuer;
        bool proves;
    };
    const Arm arms[] = {
        {"fixed key, no key id", &nakama, true, "", true, true},
        {"fixed key ignores a key id", &nakama, true, "unlisted", true, true},
        {"fixed key, another signer", &nakama, true, "", false, false},
        {"no fixed key, no key id", &nakama, false, "", true, false},
        {"activity worker token", &worker, true, "", true, true},
        {"activity worker token, another signer",
         &worker,
         true,
         "",
         false,
         false},
    };
    for (const Arm &arm : arms) {
        const Shape &shape = *arm.shape;
        Dictionary claims = with({
            {"iss", String(shape.iss)},
            {"sub", String(shape.sub)},
            {"name", String(shape.name)},
            {"aud", String(shape.aud)},
            {"nonce", p_keys.proof->save_to_string(true).sha256_text()},
            {"iat", NOW - 10},
            {"exp", NOW + 3590},
        });
        if (String(shape.instance).length() > 0) {
            claims["instance"] = String(shape.instance);
        }
        Ref<netw::NetwIdToken> server;
        server.instantiate();
        server->set_issuer(shape.iss);
        server->set_audience(shape.aud);
        server->set_prefix(shape.prefix);
        server->keys_answered(
            id_token::KEYS_RESULT_SUCCESS,
            200,
            jwks_of(listing({jwk_of("listed", modulus_of(p_keys.issuer))})),
            0
        );
        if (arm.fixed_key) {
            server->set_key(public_key(p_keys.issuer));
        }
        Dictionary header = with({
            {"alg", String("RS256")},
            {"typ", String("JWT")},
        });
        if (String(arm.kid).length() > 0) {
            header["kid"] = String(arm.kid);
        }
        const String made = signed_token(
            header,
            claims,
            arm.signed_by_issuer ? p_keys.issuer : p_keys.other
        );
        const Ref<netw::NetwJoinRequest> request
            = request_for(String(CHALLENGE));

        const Variant answer = server->verify_at(
            request,
            proof_of(made, p_keys.proof, String(CHALLENGE)),
            NOW,
            1
        );

        const PackedStringArray proven = request->get_accounts();
        if (!answered_ok(answer) || proven.size() != (arm.proves ? 1 : 0)) {
            return law_broken("%s proved %d", arm.label, int(proven.size()));
        }
        if (arm.proves
            && (proven[0] != String(shape.prefix) + ":" + String(shape.sub)
                || request->name_of(proven[0]) != String(shape.name))) {
            return law_broken("%s proved another account", arm.label);
        }
    }
    return law_held();
}

class FetchingIdToken : public netw::NetwIdToken {
protected:
    Error request_keys() override {
        return OK;
    }
};

LawVerdict rotated_key(const Keys &p_keys) {
    const Ref<FetchingIdToken> server(memnew(FetchingIdToken));
    server->set_issuer("https://discord.com");
    server->set_audience(APP);
    server->set_prefix("discord");
    server->set_keys_url("https://keys.invalid/jwks");
    const PackedByteArray listed = jwks_of(
        listing({jwk_of("fixture-rsa", modulus_of(p_keys.issuer))})
    );
    const PackedByteArray rotated = jwks_of(listing({
        jwk_of("fixture-rsa", modulus_of(p_keys.issuer)),
        jwk_of("rotated-key", modulus_of(p_keys.other)),
    }));
    server->keys_answered(0, 200, listed, ticks());
    const auto proof_by = [&](const char *p_kid) {
        return proof_of(
            signed_token(
                with({{"alg", String("RS256")}, {"kid", String(p_kid)}}),
                discord_claims(p_keys),
                p_keys.other
            ),
            p_keys.proof,
            String(CHALLENGE)
        );
    };

    const Ref<netw::NetwJoinRequest> first = request_for(String(CHALLENGE));
    const Ref<netw::NetwJoinRequest> second = request_for(String(CHALLENGE));
    const Ref<netw::NetwPromise> first_wait = promise_in(
        server->verify_at(first, proof_by("rotated-key"), NOW, ticks())
    );
    const Ref<netw::NetwPromise> second_wait = promise_in(
        server->verify_at(second, proof_by("rotated-key"), NOW, ticks())
    );
    server->keys_answered(0, 200, rotated, ticks());
    if (first_wait.is_null() || !first_wait->get_is_completed()
        || second_wait.is_null() || !second_wait->get_is_completed()) {
        return law_broken("a join waiting on the key set never settled");
    }
    if (first->get_accounts().size() != 1
        || second->get_accounts().size() != 1) {
        return law_broken("a token signed by the rotated key proved nothing");
    }

    const Ref<netw::NetwJoinRequest> unknown = request_for(String(CHALLENGE));
    const Ref<netw::NetwPromise> unknown_wait = promise_in(
        server->verify_at(unknown, proof_by("unknown-key"), NOW, ticks())
    );
    server->keys_answered(0, 200, rotated, ticks());
    if (unknown_wait.is_null() || !unknown_wait->get_is_completed()) {
        return law_broken("a join waiting on a missing key never settled");
    }
    const Ref<netw::NetwJoinRequest> cooling = request_for(String(CHALLENGE));
    const Variant cooled
        = server->verify_at(cooling, proof_by("unknown-key"), NOW, ticks());
    if (!answered_ok(cooled)) {
        return law_broken("a missing key during the cooldown waited");
    }
    if (unknown->get_accounts().size() != 0
        || cooling->get_accounts().size() != 0) {
        return law_broken("a key the set lacks proved an account");
    }
    return law_held();
}

LawVerdict failed_download(const Keys &p_keys) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_warning(
        "*https://keys.invalid/jwks*[NW1000]*",
        2
    );
    const Ref<FetchingIdToken> server(memnew(FetchingIdToken));
    server->set_issuer("https://discord.com");
    server->set_audience(APP);
    server->set_prefix("discord");
    server->set_keys_url("https://keys.invalid/jwks");
    const PackedByteArray listed = jwks_of(
        listing({jwk_of("fixture-rsa", modulus_of(p_keys.issuer))})
    );
    server->keys_answered(0, 200, listed, 0);

    server->keys_answered(2, 0, PackedByteArray(), 0);
    server->keys_answered(0, 500, listed, 0);
    server->keys_answered(0, 200, jwks_of(Array()), 0);
    const Ref<netw::NetwJoinRequest> request = request_for(String(CHALLENGE));
    const String made = signed_token(
        with({{"alg", String("RS256")}, {"kid", String("fixture-rsa")}}),
        discord_claims(p_keys),
        p_keys.issuer
    );
    const Variant answer = server->verify_at(
        request,
        proof_of(made, p_keys.proof, String(CHALLENGE)),
        NOW,
        1
    );
    server->keys_answered(0, 200, listed, 0);
    server->keys_answered(2, 0, PackedByteArray(), 0);

    if (!answered_ok(answer) || request->get_accounts().size() != 1) {
        return law_broken("a trusted key stopped proving after a failure");
    }
    return law_held();
}

class TokenDesk : public RefCounted {
public:
    Ref<CryptoKey> signer;

    Variant fetch(const String &p_nonce) {
        return netw::NetwPromise::resolved(signed_token(
            with({{"alg", String("RS256")}, {"kid", String("desk")}}),
            with({
                {"iss", String("https://game.test")},
                {"aud", String(APP)},
                {"nonce", p_nonce},
                {"sub", String("desk-player")},
                {"iat", int64_t(NOW - 10)},
                {"exp", int64_t(NOW + 3600)},
            }),
            signer
        ));
    }
};

LawVerdict client_proof(const Keys &p_keys) {
    const Ref<TokenDesk> desk(memnew(TokenDesk));
    desk->signer = p_keys.issuer;
    netw::NetwIdToken::provide(
        String("https://game.test"),
        callable_mp(desk.ptr(), &TokenDesk::fetch)
    );
    Ref<netw::NetwIdToken> client;
    client.instantiate();
    client->set_issuer(String("https://game.test"));
    const Ref<netw::NetwPromise> waiting
        = promise_in(client->prove_at(String(CHALLENGE), NOW));
    if (waiting.is_null() || !waiting->get_is_completed()) {
        return law_broken("the client produced no proof");
    }
    const Variant proof = waiting->get_result();

    Ref<netw::NetwIdToken> server;
    server.instantiate();
    server->set_issuer("https://game.test");
    server->set_audience(APP);
    server->set_prefix("game");
    server->set_key(public_key(p_keys.issuer));
    const Ref<netw::NetwJoinRequest> request = request_for(String(CHALLENGE));
    const Variant answer = server->verify_at(request, proof, NOW, 1);
    netw::NetwIdToken::provide(String("https://game.test"), Callable());

    if (!answered_ok(answer)
        || request->get_accounts() != PackedStringArray({"game:desk-player"})) {
        return law_broken("the server did not prove the client's account");
    }
    return law_held();
}

const Row ROWS[] = {
    {"a token every check holds for",
     "it proves its account and name, within a minute of clock skew",
     &holding_token},
    {"a token with one claim, header field or signature wrong, or stale",
     "it proves no account",
     &forged_or_stale_token},
    {"a token with no key id",
     "it is checked against the fixed key, and proves nothing without one",
     &fixed_key},
    {"a token signed by a key the server has not downloaded",
     "it proves once the key set holds that key, and never while it lacks it",
     &rotated_key},
    {"a key download that fails",
     "the trusted keys keep proving and each failure streak warns once",
     &failed_download},
    {"a client's proof from its token source",
     "a server holding the issuer's key proves that account",
     &client_proof},
};

TEST_CASE("[Networked][IdToken] a login token proves an account or nothing") {
    const Keys keys = {
        String("tokens signed by an issuer"),
        private_key(netw_test::ISSUER_RSA_KEY),
        private_key(netw_test::OTHER_RSA_KEY),
        private_key(netw_test::PROOF_RSA_KEY),
    };
    for (const Row &row : ROWS) {
        NETW_CELL(row, keys);
        NETW_LAW_HOLDS(row, keys);
    }
}

} // namespace TestNetwIdToken
