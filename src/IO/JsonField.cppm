/// @file JsonField.cppm
/// @brief JSONフィールドの定義。構造体とJSONの相互変換を提供する。

module;
#include <memory>
#include <concepts>
#include <tuple>
#include <type_traits>
#include <utility>
#include <string>
#include <string_view>
#include <stdexcept>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <variant>
#include <bitset>
#include <functional>
#include <ranges>
#include <typeinfo>
#include <vector>
#include <set>
#include <unordered_set>

export module rai.json.json_field;

import rai.json.json_concepts;
import rai.json.json_writer;
import rai.json.json_parser;
import rai.json.json_token_manager;
import rai.collection.sorted_hash_array_map;

namespace rai::json {

// ******************************************************************************** メタプログラミング用の型特性

/// @brief メンバーポインタの特性を抽出するメタ関数。
/// @tparam T メンバーポインタ型。
export template <typename T>
struct MemberPointerTraits;

template <typename Owner, typename Value>
struct MemberPointerTraits<Value Owner::*> {
    using OwnerType = Owner;
    using ValueType = Value;
};

/// @brief ポインタ型から要素型を抽出するメタ関数。
/// @tparam T ポインタ型（unique_ptr<T>、shared_ptr<T>、T*）。
export template <typename T>
struct PointerElementType;

template <typename T>
struct PointerElementType<std::unique_ptr<T>> {
    using type = T;
};

template <typename T>
struct PointerElementType<std::shared_ptr<T>> {
    using type = T;
};

template <typename T>
struct PointerElementType<T*> {
    using type = T;
};

/// @brief ポインタ型（unique_ptr/shared_ptr/生ポインタ）であることを確認するconcept。
export template <typename T>
concept SmartOrRawPointer = requires {
    typename PointerElementType<T>::type;
} && (std::is_same_v<T, std::unique_ptr<typename PointerElementType<T>::type>> ||
      std::is_same_v<T, std::shared_ptr<typename PointerElementType<T>::type>> ||
      std::is_same_v<T, typename PointerElementType<T>::type*>);

/// @brief ポインタ型のvectorであることを確認するconcept。
export template <typename T>
concept VectorOfPointers = requires {
    typename T::value_type;
} && SmartOrRawPointer<typename T::value_type> &&
     (std::is_same_v<T, std::vector<typename T::value_type>>);

/// @brief std::vector型を判定するメタ関数。
export template <typename T>
struct IsStdVector : std::false_type {};

template <typename U, typename Alloc>
struct IsStdVector<std::vector<U, Alloc>> : std::true_type {};

/// @brief std::variant型を判定するメタ関数。
export template <typename T>
struct IsStdVariant : std::false_type {};

template <typename... Types>
struct IsStdVariant<std::variant<Types...>> : std::true_type {};

/// @brief std::unique_ptr型を判定するconcept。
export template <typename T>
concept UniquePointer = std::is_same_v<std::remove_cvref_t<T>,
    std::unique_ptr<typename PointerElementType<std::remove_cvref_t<T>>::type>>;

/// @brief 文字列系型かどうかを判定するconcept。
export template <typename T>
concept StringLike = std::is_same_v<std::remove_cvref_t<T>, std::string> ||
    std::is_same_v<std::remove_cvref_t<T>, std::string_view>;

/// @brief 常にfalseを返す補助変数テンプレート。
export template <typename>
inline constexpr bool AlwaysFalse = false;

/// @note nullは全ポインタ型（unique_ptr/shared_ptr/生ポインタ）へ暗黙変換可能のため、
///       専用ヘルパーは不要（nullptrを直接返す）。

// ******************************************************************************** フィールド定義

/// @brief JSONフィールドの基本定義。
/// @tparam MemberPtr メンバー変数へのポインタ。
export template <typename MemberPtrType>
struct JsonFieldBase {
    static_assert(std::is_member_object_pointer_v<MemberPtrType>,
        "JsonField requires a data member pointer");
    using Traits = MemberPointerTraits<MemberPtrType>;
    using OwnerType = typename Traits::OwnerType;
    using ValueType = typename Traits::ValueType;

    /// @brief コンストラクタ。
    /// @param memberPtr メンバー変数へのポインタ。
    /// @param keyName JSONキー名。
    /// @param req 必須フィールドかどうか。
    constexpr explicit JsonFieldBase(MemberPtrType memberPtr, const char* keyName, bool req = false)
        : member(memberPtr), key(keyName), required(req) {}

    MemberPtrType member{}; ///< メンバー変数へのポインタ。
    const char* key{};      ///< JSONキー名。
    bool required{false};   ///< 必須フィールドかどうか。

protected:
    // 以下は型に依存しないユーティリティを提供する

    /// @brief 値の書き出しユーティリティ（基本型）
    template <typename T>
        requires IsFundamentalValue<T>
    static void writeValue(JsonWriter& writer, const T& value) {
        writer.writeObject(value);
    }

    /// @brief 文字列型の書き出し（std::string）
    template <typename T>
        requires std::is_same_v<std::remove_cvref_t<T>, std::string>
    static void writeValue(JsonWriter& writer, const T& value) {
        writer.writeObject(value);
    }

    /// @brief unique_ptr系の書き出し
    template <typename T>
        requires UniquePointer<T>
    static void writeValue(JsonWriter& writer, const T& ptr) {
        if (!ptr) {
            writer.null();
            return;
        }
        using Element = typename PointerElementType<std::remove_cvref_t<T>>::type;
        if constexpr (HasJsonFields<Element>) {
            auto& fields = ptr->jsonFields();
            writer.startObject();
            fields.writeFieldsOnly(writer, ptr.get());
            writer.endObject();
        } else {
            writeValue(writer, *ptr);
        }
    }

    /// @brief variantの書き出し。
    template <typename T>
        requires IsStdVariant<std::remove_cvref_t<T>>::value
    static void writeValue(JsonWriter& writer, const T& v) {
        std::visit([&writer](const auto& inner) { writeValue(writer, inner); }, v);
    }

    /// @brief range型の書き出し（string を除外）
    template <typename T>
        requires (std::ranges::range<T> && !StringLike<T>)
    static void writeValue(JsonWriter& writer, const T& range) {
        writer.startArray();
        for (const auto& elem : range) {
            writeValue(writer, elem);
        }
        writer.endArray();
    }

    /// @brief jsonFieldsを持つ型の書き出し
    template <typename T>
        requires HasJsonFields<T>
    static void writeValue(JsonWriter& writer, const T& obj) {
        auto& fields = obj.jsonFields();
        writer.startObject();
        fields.writeFieldsOnly(writer, static_cast<const void*>(&obj));
        writer.endObject();
    }

    /// @brief writeJson を持つ型の書き出し
    template <typename T>
        requires HasWriteJson<T>
    static void writeValue(JsonWriter& writer, const T& obj) {
        obj.writeJson(writer);
    }

    /// @brief 読み取りユーティリティ（基本型）
    template <typename T>
        requires IsFundamentalValue<std::remove_cvref_t<T>>
    static std::remove_cvref_t<T> readValue(JsonParser& parser) {
        std::remove_cvref_t<T> out{};
        parser.readTo(out);
        return out;
    }


    /// @brief readJsonを持つ型の読み取り
    template <typename T>
        requires HasReadJson<std::remove_cvref_t<T>>
    static std::remove_cvref_t<T> readValue(JsonParser& parser) {
        std::remove_cvref_t<T> out{};
        out.readJson(parser);
        return out;
    }

    /// @brief std::string の読み取り
    template <typename T>
        requires std::is_same_v<std::remove_cvref_t<T>, std::string>
    static std::string readValue(JsonParser& parser) {
        std::string out;
        parser.readTo(out);
        return out;
    }

    /// @brief jsonFieldsを持つ型の読み取り
    template <typename T>
        requires HasJsonFields<std::remove_cvref_t<T>>
    static std::remove_cvref_t<T> readValue(JsonParser& parser) {
        return readObjectWithFields<std::remove_cvref_t<T>>(parser);
    }

    /// @brief unique_ptr系の読み取り
    template <typename T>
        requires UniquePointer<std::remove_cvref_t<T>>
    static std::remove_cvref_t<T> readValue(JsonParser& parser) {
        using Element = typename PointerElementType<std::remove_cvref_t<T>>::type;
        if (parser.nextIsNull()) {
            parser.skipValue();
            return nullptr;
        }
        if constexpr (std::is_same_v<Element, std::string>) {
            std::string tmp;
            parser.readTo(tmp);
            return std::make_unique<Element>(std::move(tmp));
        } else {
            auto elem = readValue<Element>(parser);
            return std::make_unique<Element>(std::move(elem));
        }
    }

    /// @brief range系の読み取り（vector/set等）
    template <typename T>
        requires (std::ranges::range<std::remove_cvref_t<T>> && !StringLike<std::remove_cvref_t<T>>)
    static std::remove_cvref_t<T> readValue(JsonParser& parser) {
        using Decayed = std::remove_cvref_t<T>;
        using Element = std::ranges::range_value_t<Decayed>;
        Decayed out{};
        parser.startArray();
        while (!parser.nextIsEndArray()) {
            if constexpr (std::is_same_v<Element, std::string>) {
                std::string tmp;
                parser.readTo(tmp);
                if constexpr (requires(Decayed& c, Element&& v) { c.push_back(std::move(v)); }) {
                    out.push_back(std::move(tmp));
                } else if constexpr (requires(Decayed& c, Element&& v) { c.insert(std::move(v)); }) {
                    out.insert(std::move(tmp));
                } else {
                    static_assert(AlwaysFalse<Decayed>, "Container must support push_back or insert");
                }
            } else {
                if constexpr (requires(Decayed& c, Element&& v) { c.push_back(std::move(v)); }) {
                    out.push_back(readValue<Element>(parser));
                } else if constexpr (requires(Decayed& c, Element&& v) { c.insert(std::move(v)); }) {
                    out.insert(readValue<Element>(parser));
                } else {
                    static_assert(AlwaysFalse<Decayed>, "Container must support push_back or insert");
                }
            }
        }
        parser.endArray();
        return out;
    }

    /// @brief variant の読み取り
    template <typename T>
        requires IsStdVariant<std::remove_cvref_t<T>>::value
    static std::remove_cvref_t<T> readValue(JsonParser& parser) {
        return readVariant<std::remove_cvref_t<T>>(parser);
    }




private:
    /// @brief jsonFields()を持つ型の読み取りを行う。
    /// @tparam T 読み取り対象の型。
    /// @param parser JsonParserの参照。
    /// @return 読み取ったオブジェクト。
    template <typename T>
    static T readObjectWithFields(JsonParser& parser) {
        T obj{};
        auto& fields = obj.jsonFields();
        parser.startObject();
        while (!parser.nextIsEndObject()) {
            std::string key = parser.nextKey();
            if (!fields.readFieldByKey(parser, &obj, key)) {
                parser.noteUnknownKey(key);
                parser.skipValue();
            }
        }
        parser.endObject();
        return obj;
    }

    /// @brief variantの読み取りを行う。
    /// @tparam VariantType variant型。
    /// @param parser JsonParserの参照。
    /// @return 読み取ったvariant値。
    template <typename VariantType>
    static VariantType readVariant(JsonParser& parser) {
        auto tokenType = parser.nextTokenType();
        return readVariantImpl<VariantType>(parser, tokenType);
    }

    /// @brief variant読み取りの実装。
    /// @tparam VariantType variant型。
    /// @tparam Index 現在検査中のインデックス。
    /// @param parser JsonParserの参照。
    /// @param tokenType 先読みしたトークン種別。
    /// @return 読み取ったvariant値。
    template <typename VariantType, std::size_t Index = 0>
    static VariantType readVariantImpl(JsonParser& parser, JsonTokenType tokenType) {
        constexpr std::size_t AlternativeCount = std::variant_size_v<VariantType>;
        if constexpr (Index >= AlternativeCount) {
            throw std::runtime_error("Failed to dispatch variant for current token");
        }
        else {
            using Alternative = std::variant_alternative_t<Index, VariantType>;
            if (isVariantAlternativeMatch<Alternative>(tokenType)) {
                return VariantType(std::in_place_index<Index>, readValue<Alternative>(parser));
            }
            return readVariantImpl<VariantType, Index + 1>(parser, tokenType);
        }
    }

    /// @brief variant代替型がトークン種別に適合するかを判定する。
    /// @tparam T 代替型。
    /// @param tokenType 判定対象のトークン種別。
    /// @return 適合する場合はtrue。
    template <typename T>
    static bool isVariantAlternativeMatch(JsonTokenType tokenType) {
        using Decayed = std::remove_cvref_t<T>;
        switch (tokenType) {
        case JsonTokenType::Null:
            return UniquePointer<Decayed>;
        case JsonTokenType::Bool:
            return std::is_same_v<Decayed, bool>;
        case JsonTokenType::Integer:
            return std::is_integral_v<Decayed> && !std::is_same_v<Decayed, bool>;
        case JsonTokenType::Number:
            return std::is_floating_point_v<Decayed>;
        case JsonTokenType::String:
            return std::is_same_v<Decayed, std::string>;
        case JsonTokenType::StartObject:
            return HasJsonFields<Decayed> || HasReadJson<Decayed> || UniquePointer<Decayed>;
        case JsonTokenType::StartArray:
            return IsStdVector<Decayed>::value;
        default:
            return false;
        }
    }

};

// Forward declaration of JsonField (primary template)
export template <typename MemberPtrType>
struct JsonField;

// Deduction guides for constructing JsonField from constructor arguments
export template <typename MemberPtrType>
JsonField(MemberPtrType, const char*, bool) -> JsonField<MemberPtrType>;
export template <typename MemberPtrType>
JsonField(MemberPtrType, const char*) -> JsonField<MemberPtrType>;

// ------------------------------
// JsonField partial specializations
// ------------------------------
// Specialization: std::string
/// Handles string member variables. Writes/reads string values directly.
export template <typename Owner, typename Value>
    requires std::same_as<std::remove_cvref_t<Value>, std::string>
struct JsonField<Value Owner::*> : JsonFieldBase<Value Owner::*> {
    using Base = JsonFieldBase<Value Owner::*>;
    using ValueType = typename Base::ValueType;

    constexpr explicit JsonField(Value Owner::* memberPtr, const char* keyName, bool req = false)
        : Base(memberPtr, keyName, req) {}

    void toJson(JsonWriter& writer, const ValueType& value) const {
        writer.writeObject(value);
    }

    ValueType fromJson(JsonParser& parser) const {
        ValueType out;
        parser.readTo(out);
        return out;
    }
};

// Specialization: fundamental types (int/float/bool/...)
/// Uses fundamental read/write helpers (readTo/writeObject).
export template <typename Owner, typename Value>
    requires IsFundamentalValue<std::remove_cvref_t<Value>>
struct JsonField<Value Owner::*> : JsonFieldBase<Value Owner::*> {
    using Base = JsonFieldBase<Value Owner::*>;
    using ValueType = typename Base::ValueType;

    constexpr explicit JsonField(Value Owner::* memberPtr, const char* keyName, bool req = false)
        : Base(memberPtr, keyName, req) {}

    void toJson(JsonWriter& writer, const ValueType& value) const {
        Base::template writeValue<ValueType>(writer, value);
    }

    ValueType fromJson(JsonParser& parser) const {
        return Base::template readValue<ValueType>(parser);
    }
};

// Specialization: unique_ptr / smart pointer types
/// Handles pointer types; supports null and object/string payloads.
export template <typename Owner, typename Value>
    requires UniquePointer<std::remove_cvref_t<Value>>
struct JsonField<Value Owner::*> : JsonFieldBase<Value Owner::*> {
    using Base = JsonFieldBase<Value Owner::*>;
    using ValueType = typename Base::ValueType;

    constexpr explicit JsonField(Value Owner::* memberPtr, const char* keyName, bool req = false)
        : Base(memberPtr, keyName, req) {}

    void toJson(JsonWriter& writer, const ValueType& value) const {
        Base::template writeValue<ValueType>(writer, value);
    }

    ValueType fromJson(JsonParser& parser) const {
        return Base::template readValue<ValueType>(parser);
    }
};

// Specialization: std::variant
/// Dispatches variant alternatives using readVariant/writeValue.
export template <typename Owner, typename Value>
    requires IsStdVariant<std::remove_cvref_t<Value>>::value
struct JsonField<Value Owner::*> : JsonFieldBase<Value Owner::*> {
    using Base = JsonFieldBase<Value Owner::*>;
    using ValueType = typename Base::ValueType;

    constexpr explicit JsonField(Value Owner::* memberPtr, const char* keyName, bool req = false)
        : Base(memberPtr, keyName, req) {}

    void toJson(JsonWriter& writer, const ValueType& value) const {
        Base::template writeValue<ValueType>(writer, value);
    }

    ValueType fromJson(JsonParser& parser) const {
        return Base::template readValue<ValueType>(parser);
    }
};

// Specialization: ranges (containers like vector, set) excluding string
/// Generic container handling; requires push_back or insert for element insertion.
export template <typename Owner, typename Value>
    requires (std::ranges::range<std::remove_cvref_t<Value>> && !StringLike<std::remove_cvref_t<Value>>)
struct JsonField<Value Owner::*> : JsonFieldBase<Value Owner::*> {
    using Base = JsonFieldBase<Value Owner::*>;
    using ValueType = typename Base::ValueType;

    constexpr explicit JsonField(Value Owner::* memberPtr, const char* keyName, bool req = false)
        : Base(memberPtr, keyName, req) {}

    void toJson(JsonWriter& writer, const ValueType& value) const {
        Base::template writeValue<ValueType>(writer, value);
    }

    ValueType fromJson(JsonParser& parser) const {
        return Base::template readValue<ValueType>(parser);
    }
};

// Specialization: types with jsonFields() or custom readJson/writeJson
/// Delegates to object-level jsonFields/readJson/writeJson implementations.
export template <typename Owner, typename Value>
    requires (HasJsonFields<std::remove_cvref_t<Value>> ||
              HasReadJson<std::remove_cvref_t<Value>> ||
              HasWriteJson<std::remove_cvref_t<Value>>)
struct JsonField<Value Owner::*> : JsonFieldBase<Value Owner::*> {
    using Base = JsonFieldBase<Value Owner::*>;
    using ValueType = typename Base::ValueType;

    constexpr explicit JsonField(Value Owner::* memberPtr, const char* keyName, bool req = false)
        : Base(memberPtr, keyName, req) {}

    void toJson(JsonWriter& writer, const ValueType& value) const {
        Base::template writeValue<ValueType>(writer, value);
    }

    ValueType fromJson(JsonParser& parser) const {
        return Base::template readValue<ValueType>(parser);
    }
};

// Fallback generic partial specialization (catch-all)
/// Generic fallback that uses writeValue/readValue helpers.
export template <typename Owner, typename Value>
struct JsonField<Value Owner::*> : JsonFieldBase<Value Owner::*> {
    using Base = JsonFieldBase<Value Owner::*>;
    using ValueType = typename Base::ValueType;

    constexpr explicit JsonField(Value Owner::* memberPtr, const char* keyName, bool req = false)
        : Base(memberPtr, keyName, req) {}

    void toJson(JsonWriter& writer, const ValueType& value) const {
        Base::template writeValue<ValueType>(writer, value);
    }

    ValueType fromJson(JsonParser& parser) const {
        return Base::template readValue<ValueType>(parser);
    }
};


}  // namespace rai::json
