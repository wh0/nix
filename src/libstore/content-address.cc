#include "args.hh"
#include "content-address.hh"
#include "split.hh"

namespace nix {

std::string FixedOutputHash::printMethodAlgo() const
{
    return makeFileIngestionPrefix(method) + printHashType(hash.type);
}


std::string makeFileIngestionPrefix(FileIngestionMethod m)
{
    switch (m) {
    case FileIngestionMethod::Flat:
        return "";
    case FileIngestionMethod::Recursive:
        return "r:";
    default:
        throw Error("impossible, caught both cases");
    }
}

std::string makeContentAddressingPrefix(ContentAddressMethod m) {
    return std::visit(overloaded {
        [](TextHashMethod) -> std::string { return "text:"; },
        [](FileIngestionMethod m2) {
             /* Not prefixed for back compat with things that couldn't produce text before. */
            return makeFileIngestionPrefix(m2);
        },
    }, m);
}

ContentAddressMethod parseContentAddressingPrefix(std::string_view & m)
{
    ContentAddressMethod method = FileIngestionMethod::Flat;
    if (splitPrefix(m, "r:"))
        method = FileIngestionMethod::Recursive;
    else if (splitPrefix(m, "text:"))
        method = TextHashMethod {};
    return method;
}


std::string makeFixedOutputCA(FileIngestionMethod method, const Hash & hash)
{
    return "fixed:"
        + makeFileIngestionPrefix(method)
        + hash.to_string(Base32, true);
}

std::string renderContentAddress(ContentAddress ca)
{
    return std::visit(overloaded {
        [](TextHash & th) {
            return "text:"
                + th.hash.to_string(Base32, true);
        },
        [](FixedOutputHash & fsh) {
            return "fixed:"
                + makeFileIngestionPrefix(fsh.method)
                + fsh.hash.to_string(Base32, true);
        }
    }, ca);
}

std::string renderContentAddressMethodAndHash(ContentAddressMethod cam, HashType ht)
{
    return std::visit(overloaded {
        [&](TextHashMethod & th) {
            return std::string{"text:"} + printHashType(ht);
        },
        [&](FileIngestionMethod & fim) {
            return "fixed:" + makeFileIngestionPrefix(fim) + printHashType(ht);
        }
    }, cam);
}

/*
  Parses content address strings up to the hash.
 */
static std::pair<ContentAddressMethod, HashType> parseContentAddressMethodPrefix(std::string_view & rest)
{
    std::string_view wholeInput { rest };

    std::string_view prefix;
    {
        auto optPrefix = splitPrefixTo(rest, ':');
        if (!optPrefix)
            throw UsageError("not a content address because it is not in the form '<prefix>:<rest>': %s", wholeInput);
        prefix = *optPrefix;
    }

    auto parseHashType_ = [&](){
        auto hashTypeRaw = splitPrefixTo(rest, ':');
        if (!hashTypeRaw)
            throw UsageError("content address hash must be in form '<algo>:<hash>', but found: %s", wholeInput);
        HashType hashType = parseHashType(*hashTypeRaw);
        return std::move(hashType);
    };

    // Switch on prefix
    if (prefix == "text") {
        // No parsing of the ingestion method, "text" only support flat.
        HashType hashType = parseHashType_();
        return {
            TextHashMethod {},
            std::move(hashType),
        };
    } else if (prefix == "fixed") {
        // Parse method
        auto method = FileIngestionMethod::Flat;
        if (splitPrefix(rest, "r:"))
            method = FileIngestionMethod::Recursive;
        HashType hashType = parseHashType_();
        return {
            std::move(method),
            std::move(hashType),
        };
    } else
        throw UsageError("content address prefix '%s' is unrecognized. Recogonized prefixes are 'text' or 'fixed'", prefix);
}

ContentAddress parseContentAddress(std::string_view rawCa) {
    auto rest = rawCa;

    auto [caMethod, hashType_] = parseContentAddressMethodPrefix(rest);
    auto hashType = hashType_; // work around clang bug

    return std::visit(overloaded {
        [&](TextHashMethod &) {
            return ContentAddress(TextHash {
                .hash = Hash::parseNonSRIUnprefixed(rest, hashType)
            });
        },
        [&](FileIngestionMethod & fim) {
            return ContentAddress(FixedOutputHash {
                .method = fim,
                .hash = Hash::parseNonSRIUnprefixed(rest, hashType),
            });
        },
    }, caMethod);
}

std::pair<ContentAddressMethod, HashType> parseContentAddressMethod(std::string_view caMethod)
{
    std::string asPrefix = std::string{caMethod} + ":";
    // parseContentAddressMethodPrefix takes its argument by reference
    std::string_view asPrefixView = asPrefix;
    return parseContentAddressMethodPrefix(asPrefixView);
}

std::optional<ContentAddress> parseContentAddressOpt(std::string_view rawCaOpt)
{
    return rawCaOpt == "" ? std::optional<ContentAddress>() : parseContentAddress(rawCaOpt);
};

std::string renderContentAddress(std::optional<ContentAddress> ca)
{
    return ca ? renderContentAddress(*ca) : "";
}

ContentAddressWithReferences contentAddressFromMethodHashAndRefs(
    ContentAddressMethod method, Hash && hash, StoreReferences && refs)
{
    return std::visit(overloaded {
        [&](TextHashMethod _) -> ContentAddressWithReferences {
            if (refs.self)
                throw UsageError("Cannot have a self reference with text hashing scheme");
            return TextInfo {
                .hash = { .hash = std::move(hash) },
                .references = std::move(refs.others),
            };
        },
        [&](FileIngestionMethod m2) -> ContentAddressWithReferences {
            return FixedOutputInfo {
                .hash = {
                    .method = m2,
                    .hash = std::move(hash),
                },
                .references = std::move(refs),
            };
        },
    }, method);
}

ContentAddressMethod getContentAddressMethod(const ContentAddressWithReferences & ca)
{
    return std::visit(overloaded {
        [](const TextInfo & th) -> ContentAddressMethod {
            return TextHashMethod {};
        },
        [](const FixedOutputInfo & fsh) -> ContentAddressMethod {
            return fsh.hash.method;
        },
    }, ca);
}

Hash getContentAddressHash(const ContentAddress & ca)
{
    return std::visit(overloaded {
        [](const TextHash & th) {
            return th.hash;
        },
        [](const FixedOutputHash & fsh) {
            return fsh.hash;
        },
    }, ca);
}

bool StoreReferences::empty() const
{
    return !self && others.empty();
}

size_t StoreReferences::size() const
{
    return (self ? 1 : 0) + others.size();
}

ContentAddressWithReferences caWithoutRefs(const ContentAddress & ca) {
    return std::visit(overloaded {
        [&](const TextHash & h) -> ContentAddressWithReferences {
            return TextInfo {
                .hash = h,
                .references = {},
            };
        },
        [&](const FixedOutputHash & h) -> ContentAddressWithReferences {
            return FixedOutputInfo {
                .hash = h,
                .references = {},
            };
        },
    }, ca);
}

Hash getContentAddressHash(const ContentAddressWithReferences & ca)
{
    return std::visit(overloaded {
        [](const TextInfo & th) {
            return th.hash.hash;
        },
        [](const FixedOutputInfo & fsh) {
            return fsh.hash.hash;
        },
    }, ca);
}

std::string printMethodAlgo(const ContentAddressWithReferences & ca) {
    return makeContentAddressingPrefix(getContentAddressMethod(ca))
        + printHashType(getContentAddressHash(ca).type);
}

}
