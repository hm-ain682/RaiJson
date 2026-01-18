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
import rai.json.json_value_io;
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

/// @brief string 系を除くレンジ（配列/コンテナ）を表す concept。
/// @details std::ranges::range を満たし、かつ `StringLike` を除外することで
///          `std::string` を配列として誤判定しないようにします。
export template<typename T>
concept RangeContainer = std::ranges::range<T> && !StringLike<T>;

/// @brief `value_io` が直接扱える型群を表す concept。
/// @details 含まれる型の代表例:
///   - 基本型（数値/真偽値 等）
///   - `std::string`
///   - ユニークポインタ等のスマートポインタ（`UniquePointer`）
///   - `std::variant`（`IsStdVariant`）
///   - レンジコンテナ（`RangeContainer`）
///   - `jsonFields()` を提供するオブジェクト（`HasJsonFields`）
///   - `readJson` / `writeJson` を持つ型（`HasReadJson && HasWriteJson`）
/// @note 新しい型サポートを追加する際は、`value_io` 側の実装と
///       両方を更新してください。これにより診断が早期に行われます。
export template<typename T>
concept ValueIoSupported =
    IsFundamentalValue<T> ||
    std::same_as<T, std::string> ||
    UniquePointer<T> ||
    IsStdVariant<T>::value ||
    RangeContainer<T> ||
    HasJsonFields<T> ||
    (HasReadJson<T> && HasWriteJson<T>);

/// @brief `value_io` に処理を委譲する `JsonField` の部分特殊化。
/// @details `ValueIoSupported` を満たす型のみを受け付け、
///          `value_io::writeValue` / `value_io::readValue` に処理を委ねます。
///          未対応型は concept によってコンパイル時に早期に検出され、
///          エラーの発生箇所と原因が明確になります。
export template <typename Owner, typename Value>
    requires ValueIoSupported<std::remove_cvref_t<Value>>
struct JsonField<Value Owner::*> : JsonFieldBase<Value Owner::*> {
    using Base = JsonFieldBase<Value Owner::*>;
    using ValueType = typename Base::ValueType;

    constexpr explicit JsonField(Value Owner::* memberPtr, const char* keyName, bool req = false)
        : Base(memberPtr, keyName, req) {}

    void toJson(JsonWriter& writer, const ValueType& value) const {
        value_io::template writeValue<ValueType>(writer, value);
    }

    ValueType fromJson(JsonParser& parser) const {
        return value_io::template readValue<ValueType>(parser);
    }
};

}  // namespace rai::json
