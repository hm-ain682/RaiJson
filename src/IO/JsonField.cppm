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
template <typename T>
struct MemberPointerTraits;

template <typename Owner, typename Value>
struct MemberPointerTraits<Value Owner::*> {
    using OwnerType = Owner;
    using ValueType = Value;
};

/// @brief ポインタ型から要素型を抽出するメタ関数。
/// @tparam T ポインタ型（unique_ptr<T>、shared_ptr<T>、T*）。
template <typename T>
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
template <typename T>
concept SmartOrRawPointer = requires {
    typename PointerElementType<T>::type;
} && (std::is_same_v<T, std::unique_ptr<typename PointerElementType<T>::type>> ||
      std::is_same_v<T, std::shared_ptr<typename PointerElementType<T>::type>> ||
      std::is_same_v<T, typename PointerElementType<T>::type*>);

/// @brief ポインタ型のvectorであることを確認するconcept。
template <typename T>
concept VectorOfPointers = requires {
    typename T::value_type;
} && SmartOrRawPointer<typename T::value_type> &&
     (std::is_same_v<T, std::vector<typename T::value_type>>);

/// @note nullは全ポインタ型（unique_ptr/shared_ptr/生ポインタ）へ暗黙変換可能のため、
///       専用ヘルパーは不要（nullptrを直接返す）。

// ******************************************************************************** フィールド定義

/// @brief JSONフィールドの基本定義。
/// @tparam MemberPtr メンバー変数へのポインタ。
export template <typename MemberPtrType>
struct JsonField {
    static_assert(std::is_member_object_pointer_v<MemberPtrType>,
        "JsonField requires a data member pointer");
    using Traits = MemberPointerTraits<MemberPtrType>;
    using OwnerType = typename Traits::OwnerType;
    using ValueType = typename Traits::ValueType;

    /// @brief コンストラクタ。
    /// @param memberPtr メンバー変数へのポインタ。
    /// @param keyName JSONキー名。
    /// @param req 必須フィールドかどうか。
    constexpr explicit JsonField(MemberPtrType memberPtr, const char* keyName, bool req = false)
        : member(memberPtr), key(keyName), required(req) {}

    MemberPtrType member{}; ///< メンバー変数へのポインタ。
    const char* key{};      ///< JSONキー名。
    bool required{false};   ///< 必須フィールドかどうか。
};

/// @brief Enumと文字列のマッピングエントリ。
/// @tparam EnumType 対象のenum型。
export template <typename EnumType>
struct EnumEntry {
    EnumType value;   ///< Enum値。
    const char* name; ///< 対応する文字列名。
};

/// @brief Enum型のフィールド用に特化したJsonField派生クラス。
/// @tparam MemberPtr Enumメンバー変数へのポインタ。
/// @tparam Entries Enumと文字列のマッピング配列への参照。
export template <typename MemberPtrType, std::size_t N = 0>
struct JsonEnumField : JsonField<MemberPtrType> {
    using Base = JsonField<MemberPtrType>;
    using typename Base::ValueType;
    static_assert(std::is_enum_v<ValueType>, "JsonEnumField requires enum type");

    /// @brief Enum用フィールドのコンストラクタ（エントリ配列を受け取る）
    /// @param memberPtr ポインタメンバ
    /// @param keyName JSONキー名
    /// @param entries エントリ配列
    constexpr explicit JsonEnumField(MemberPtrType memberPtr, const char* keyName,
        const EnumEntry<ValueType> (&entries)[N], bool req = false)
        : Base(memberPtr, keyName, req) {

        // build name -> value descriptor array
        std::pair<std::string_view, ValueType> nv[N];
        for (std::size_t i = 0; i < N; ++i) {
            nv[i] = { entries[i].name, entries[i].value };
        }
        nameToValue_ = collection::SortedHashArrayMap<std::string_view, ValueType, N>(nv);

        // build value -> name descriptor array
        std::pair<ValueType, std::string_view> vn[N];
        for (std::size_t i = 0; i < N; ++i) {
            vn[i] = { entries[i].value, entries[i].name };
        }
        valueToName_ = collection::SortedHashArrayMap<ValueType, std::string_view, N>(vn);
    }

    /// @brief Enum値をJSONに書き出す。
    /// @param writer JsonWriterの参照。
    /// @param value 変換対象のenum値。
    /// @throws std::runtime_error マッピングが存在しない場合。
    void toJson(JsonWriter& writer, const ValueType& value) const {
        // エントリが空の場合は常に失敗
        if constexpr (N == 0) {
            throw std::runtime_error("Failed to convert enum to string");
        }
        const auto* found = valueToName_.findValue(value);
        if (found) {
            writer.writeObject(*found);
            return;
        }
        // マッピングに存在しない値の場合
        throw std::runtime_error("Failed to convert enum to string");
    }

    /// @brief JSONからEnum値を読み取る。
    /// @param parser JsonParserの参照。
    /// @return 変換されたenum値。
    /// @throws std::runtime_error マッピングに存在しない文字列の場合。
    /// @note 内部で文字列を読み取り、enumエントリで検索する。
    ValueType fromJson(JsonParser& parser) const {
        std::string jsonValue;
        parser.readTo(jsonValue);

        // エントリが空の場合は常に失敗
        if constexpr (N == 0) {
            throw std::runtime_error(std::string("Failed to convert string to enum: ") + jsonValue);
        }
        const auto* found = nameToValue_.findValue(jsonValue);
        if (found) {
            return *found;
        }
        // マッピングに存在しない文字列の場合
        throw std::runtime_error(std::string("Failed to convert string to enum: ") + jsonValue);
    }

private:
    /// @note エントリはSortedHashArrayMapに保持する（キー->値 / 値->キーで2方向検索を高速化）
    collection::SortedHashArrayMap<std::string_view, ValueType, N> nameToValue_{}; ///< 名前からenum値へのマップ。
    collection::SortedHashArrayMap<ValueType, std::string_view, N> valueToName_{}; ///< enum値から名前へのマップ。
};

/// @brief ポリモーフィック型用のファクトリ関数型（ポインタ型を返す）。
export template <typename PtrType>
    requires SmartOrRawPointer<PtrType>
using PolymorphicTypeFactory = std::function<PtrType()>;

/// @brief ポリモーフィックオブジェクト1つ分を読み取るヘルパー関数。
/// @tparam PtrType ポインタ型（unique_ptr/shared_ptr/生ポインタ）。
/// @param parser JsonParserの参照。
/// @param entriesMap 型名からファクトリ関数へのマッピング。
/// @param jsonKey 型判別用のJSONキー名。
/// @return 読み取ったオブジェクトのポインタ。
/// @throws std::runtime_error 型キーが見つからない、または未知の型名の場合。
export template <typename PtrType>
    requires SmartOrRawPointer<PtrType>
PtrType readPolymorphicInstance(
    JsonParser& parser,
    const collection::MapReference<std::string_view, PolymorphicTypeFactory<PtrType>>& entriesMap,
    std::string_view jsonKey = "type") {

    parser.startObject();

    // 最初のキーが型判別キーであることを確認
    std::string typeKey = parser.nextKey();
    if (typeKey != jsonKey) {
        throw std::runtime_error(
            std::string("Expected '") + std::string(jsonKey) +
            "' key for polymorphic object, got '" + typeKey + "'");
    }

    // 型名を読み取り、対応するファクトリを検索
    std::string typeName;
    parser.readTo(typeName);
    const auto* factory = entriesMap.findValue(typeName);
    if (!factory) {
        throw std::runtime_error(std::string("Unknown polymorphic type: ") + typeName);
    }

    // ファクトリでインスタンスを生成
    auto instance = (*factory)();
    using BaseType = typename PointerElementType<PtrType>::type;

    // HasJsonFieldsを持つ型の場合、残りのフィールドを読み取る
    if constexpr (HasJsonFields<BaseType>) {
        auto& fields = instance->jsonFields();
        BaseType* raw = std::to_address(instance);
        while (!parser.nextIsEndObject()) {
            std::string key = parser.nextKey();
            if (!fields.readFieldByKey(parser, raw, key)) {
                // 未知のキーはスキップ
                parser.noteUnknownKey(key);
                parser.skipValue();
            }
        }
    }
    else {
        // jsonFieldsを持たない型の場合、全フィールドをスキップ
        while (!parser.nextIsEndObject()) {
            std::string key = parser.nextKey();
            parser.noteUnknownKey(key);
            parser.skipValue();
        }
    }

    parser.endObject();
    return instance;
}

/// @brief ポリモーフィックオブジェクト1つ分を読み取るヘルパー関数（null許容版）。
/// @tparam PtrType ポインタ型（unique_ptr/shared_ptr/生ポインタ）。
/// @param parser JsonParserの参照。
/// @param entriesMap 型名からファクトリ関数へのマッピング。
/// @param jsonKey 型判別用のJSONキー名。
/// @return 読み取ったオブジェクトのポインタ。nullの場合はnullptr。
template <typename PtrType>
    requires SmartOrRawPointer<PtrType>
PtrType readPolymorphicInstanceOrNull(
    JsonParser& parser,
    const collection::MapReference<std::string_view, PolymorphicTypeFactory<PtrType>>& entriesMap,
    std::string_view jsonKey = "type") {

    // null値の場合はnullptrを返す
    if (parser.nextIsNull()) {
        parser.skipValue();
        return nullptr;
    }
    // オブジェクトの場合は通常の読み取り処理
    return readPolymorphicInstance<PtrType>(parser, entriesMap, jsonKey);
}

/// @brief ポリモーフィック型（unique_ptr<基底クラス>）用のJsonField派生クラス。
/// @tparam MemberPtr unique_ptr<基底クラス>メンバー変数へのポインタ。
/// @tparam Entries 型名とファクトリ関数のマッピング配列への参照。
export template <typename MemberPtrType>
    requires SmartOrRawPointer<typename JsonField<MemberPtrType>::ValueType>
struct JsonPolymorphicField : JsonField<MemberPtrType> {
    using Base = JsonField<MemberPtrType>;
    using typename Base::ValueType;
    using BaseType = typename PointerElementType<ValueType>::type;
    using Key = std::string_view;
    using Value = PolymorphicTypeFactory<ValueType>;
    using Map = collection::MapReference<Key, Value>;

    /// @brief ポリモーフィック型用フィールドのコンストラクタ。
    /// @param memberPtr メンバー変数へのポインタ。
    /// @param keyName JSONキー名。
    /// @param entries 型名からファクトリ関数へのマッピング。
    /// @param jsonKey JSON内で型を判別するためのキー名。
    /// @param req 必須フィールドかどうか。
    constexpr explicit JsonPolymorphicField(MemberPtrType memberPtr, const char* keyName,
        Map entries, const char* jsonKey = "type", bool req = true)
        : Base(memberPtr, keyName, req), nameToEntry_(entries), jsonKey_(jsonKey) {}

    /// @brief ポリモーフィック型用フィールドのコンストラクタ（SortedHashArrayMap版）。
    /// @param memberPtr メンバー変数へのポインタ。
    /// @param keyName JSONキー名。
    /// @param entries 型名からファクトリ関数へのマッピング（SortedHashArrayMap）。
    /// @param jsonKey JSON内で型を判別するためのキー名。
    /// @param req 必須フィールドかどうか。
    template <size_t N, typename Traits>
    constexpr explicit JsonPolymorphicField(MemberPtrType memberPtr, const char* keyName,
        const collection::SortedHashArrayMap<Key, Value, N, Traits>& entries,
        const char* jsonKey = "type", bool req = true)
        : Base(memberPtr, keyName, req), nameToEntry_(entries), jsonKey_(jsonKey) {}

    /// @brief 型名から対応するエントリを検索する。
    /// @param typeName 検索する型名。
    /// @return 見つかった場合はエントリへのポインタ、見つからない場合はnullptr。
    const PolymorphicTypeFactory<ValueType>* findEntry(std::string_view typeName) const {
        return nameToEntry_.findValue(typeName);
    }

    /// @brief オブジェクトから型名を取得する。
    /// @param obj 対象オブジェクト。
    /// @return 型名。
    /// @throws std::runtime_error マッピングに存在しない型の場合。
    std::string getTypeName(const BaseType& obj) const {
        // 全エントリを走査し、typeidで一致する型名を検索
        for (const auto& it : nameToEntry_) {
            auto testObj = it.value();
            if (typeid(obj) == typeid(*testObj)) {
                return std::string(it.key);
            }
        }
        // マッピングに存在しない型の場合
        throw std::runtime_error(std::string("Unknown polymorphic type: ") + typeid(obj).name());
    }

    /// @brief JsonParser から polymorphic オブジェクトを読み込む。
    /// @param parser JsonParser の参照。現在の位置にオブジェクトか null があることを期待する。
    /// @return 要素型 T を保持するポインタ（unique_ptr/shared_ptr/生ポインタ）。null の場合は nullptr を返す。
    ValueType fromJson(JsonParser& parser) const {
        return readPolymorphicInstanceOrNull<ValueType>(parser, nameToEntry_, jsonKey_);
    }

    /// @brief JsonWriter に対して polymorphic オブジェクトを書き出す。
    /// @param writer JsonWriter の参照。
    /// @param ptr 書き込み対象の unique_ptr（null 可）。
    void toJson(JsonWriter& writer, const ValueType& ptr) const {
        if (!ptr) {
            writer.null();
            return;
        }
        writer.startObject();
        std::string typeName = getTypeName(*ptr);
        writer.key(jsonKey_);
        writer.writeObject(typeName);
        auto& fields = ptr->jsonFields();
        fields.writeFieldsOnly(writer, std::to_address(ptr));
        writer.endObject();
    }

private:
    Map nameToEntry_;     ///< 型名からファクトリ関数へのマッピング。
    const char* jsonKey_; ///< JSON内で型を判別するためのキー名。
};

/// @brief 配列形式のJSONを読み書きするための汎用フィールド。
/// @tparam MemberPtrType コンテナ型のメンバー変数へのポインタ。
/// @tparam AddElementType コンテナに要素を追加するファンクタ型。
/// @tparam ReadElementType 要素を読み取るファンクタ型。
/// @tparam WriteElementType 要素を書き出すファンクタ型。
/// @details fromJson/toJsonにおける要素の読み書き方法をカスタマイズ可能。
///          vector以外のコンテナにも対応できる汎用設計。
export template <typename MemberPtrType, typename AddElementType, typename ReadElementType,
    typename WriteElementType>
    requires std::ranges::range<typename JsonField<MemberPtrType>::ValueType>
struct JsonSetField : JsonField<MemberPtrType> {
    using Base = JsonField<MemberPtrType>;
    using typename Base::ValueType;
    using ElementType = std::ranges::range_value_t<ValueType>; ///< コンテナの要素型。

    /// @brief コンストラクタ。
    /// @param memberPtr メンバーポインタ。
    /// @param keyName JSONキー名。
    /// @param addElement コンテナに要素を追加するファンクタ。
    /// @param readElement 要素を読み取るファンクタ。
    /// @param writeElement 要素を書き出すファンクタ。
    /// @param req 必須フィールドかどうか。
    constexpr explicit JsonSetField(MemberPtrType memberPtr, const char* keyName,
        AddElementType addElement, ReadElementType readElement,
        WriteElementType writeElement, bool req = false)
        : Base(memberPtr, keyName, req),
          addElement_(std::move(addElement)),
          readElement_(std::move(readElement)),
          writeElement_(std::move(writeElement)) {}

    /// @brief JsonParser から配列を読み込む。
    /// @param parser JsonParser の参照。現在の位置に配列があることを期待する。
    /// @return 読み込まれたコンテナ。
    ValueType fromJson(JsonParser& parser) const {
        ValueType out{};
        parser.startArray();
        while (!parser.nextIsEndArray()) {
            auto elem = readElement_(parser);
            addElement_(out, std::move(elem));
        }
        parser.endArray();
        return out;
    }

    /// @brief JsonWriter に対して配列を書き出す。
    /// @param writer JsonWriter の参照。
    /// @param container 書き込み対象のコンテナ。
    void toJson(JsonWriter& writer, const ValueType& container) const {
        writer.startArray();
        for (const auto& elem : container) {
            writeElement_(writer, elem);
        }
        writer.endArray();
    }

private:
    AddElementType addElement_;    ///< コンテナに要素を追加するファンクタ。
    ReadElementType readElement_;  ///< 要素を読み取るファンクタ。
    WriteElementType writeElement_;  ///< 要素を書き出すファンクタ。
};

/// @brief JsonSetFieldを生成するヘルパー関数。
/// @tparam MemberPtrType メンバーポインタ型（自動推論）。
/// @tparam AddElementType コンテナに要素を追加するファンクタ型（自動推論）。
/// @tparam ReadElementType 要素を読み取るファンクタ型（自動推論）。
/// @tparam WriteElementType 要素を書き出すファンクタ型（自動推論）。
/// @param memberPtr メンバーポインタ。
/// @param keyName JSONキー名。
/// @param addElement コンテナに要素を追加するファンクタ。
/// @param readElement 要素を読み取るファンクタ。
/// @param writeElement 要素を書き出すファンクタ。
/// @param req 必須フィールドかどうか。
/// @return 生成されたJsonSetField。
export template <typename MemberPtrType, typename AddElementType, typename ReadElementType,
    typename WriteElementType>
constexpr auto makeJsonSetField(MemberPtrType memberPtr, const char* keyName,
    AddElementType addElement, ReadElementType readElement,
    WriteElementType writeElement, bool req = false) {
    return JsonSetField<MemberPtrType, std::remove_cvref_t<AddElementType>,
        std::remove_cvref_t<ReadElementType>, std::remove_cvref_t<WriteElementType>>(
        memberPtr, keyName, std::move(addElement), std::move(readElement),
        std::move(writeElement), req);
}

/// @brief ポリモーフィックな配列（vector<std::unique_ptr<BaseType>>）用のフィールド。
/// @tparam MemberPtrType ポインタのvector型のメンバー変数へのポインタ。
/// @details JsonSetFieldを継承し、ポリモーフィック型用のデフォルト動作を提供する。
export template <typename MemberPtrType>
    requires VectorOfPointers<typename JsonField<MemberPtrType>::ValueType>
struct JsonPolymorphicArrayField : JsonSetField<MemberPtrType,
    std::function<void(typename JsonField<MemberPtrType>::ValueType&,
                       typename JsonField<MemberPtrType>::ValueType::value_type)>,
    std::function<typename JsonField<MemberPtrType>::ValueType::value_type(JsonParser&)>,
    std::function<void(JsonWriter&,
                       const typename JsonField<MemberPtrType>::ValueType::value_type&)>> {
    using ValueType = typename JsonField<MemberPtrType>::ValueType;
    using ElementPtrType = typename ValueType::value_type; ///< std::unique_ptr<T>, std::shared_ptr<T>, or T*
    using BaseType = typename PointerElementType<ElementPtrType>::type;
    using Key = std::string_view;
    using Value = PolymorphicTypeFactory<ElementPtrType>;
    using Map = collection::MapReference<Key, Value>;

    using AddElementFunc = std::function<void(ValueType&, ElementPtrType)>;
    using ReadElementFunc = std::function<ElementPtrType(JsonParser&)>;
    using WriteElementFunc = std::function<void(JsonWriter&, const ElementPtrType&)>;
    using SetFieldBase = JsonSetField<MemberPtrType, AddElementFunc, ReadElementFunc,
        WriteElementFunc>;

    /// @brief コンストラクタ。
    /// @param memberPtr メンバーポインタ。
    /// @param keyName JSONキー名。
    /// @param entries 型名とファクトリ関数のマッピング。
    /// @param jsonKey JSON内で型を判別するためのキー名。
    /// @param req 必須フィールドかどうか。
    explicit JsonPolymorphicArrayField(MemberPtrType memberPtr, const char* keyName,
        Map entries, const char* jsonKey = "type", bool req = true)
        : SetFieldBase(memberPtr, keyName,
            addElement,
            // thisキャプチャは不可：オブジェクトがムーブされるとthisが無効になるため
            // entriesはMapReference型（軽量な参照ラッパー）のためコピーキャプチャで問題ない
            [entries, jsonKey](JsonParser& parser) {
                return readPolymorphicInstanceOrNull<ElementPtrType>(parser, entries, jsonKey);
            },
            [entries, jsonKey](JsonWriter& writer, const ElementPtrType& ptr) {
                writeElement(writer, ptr, entries, jsonKey);
            },
            req),
          nameToEntry_(entries),
          jsonKey_(jsonKey) {}

    /// @brief コンストラクタ（SortedHashArrayMap版）。
    /// @param memberPtr メンバーポインタ。
    /// @param keyName JSONキー名。
    /// @param entries 型名とファクトリ関数のマッピング（SortedHashArrayMap）。
    /// @param jsonKey JSON内で型を判別するためのキー名。
    /// @param req 必須フィールドかどうか。
    template <size_t N, typename Traits>
    explicit JsonPolymorphicArrayField(MemberPtrType memberPtr, const char* keyName,
        const collection::SortedHashArrayMap<Key, Value, N, Traits>& entries,
        const char* jsonKey = "type", bool req = true)
        : JsonPolymorphicArrayField(memberPtr, keyName, Map(entries), jsonKey, req) {}

    /// @brief 型名から対応するファクトリ関数を検索する。
    /// @param typeName 検索する型名。
    /// @return 見つかった場合はファクトリ関数へのポインタ、見つからない場合はnullptr。
    const PolymorphicTypeFactory<ElementPtrType>* findEntry(std::string_view typeName) const {
        return nameToEntry_.findValue(typeName);
    }

    /// @brief オブジェクトから型名を取得する。
    /// @param obj 対象オブジェクト。
    /// @return 型名。
    /// @throws std::runtime_error マッピングに存在しない型の場合。
    std::string getTypeName(const BaseType& obj) const {
        return getTypeNameFromMap(obj, nameToEntry_);
    }

private:
    Map nameToEntry_; ///< 型名からファクトリ関数へのマッピング。
    const char* jsonKey_; ///< JSON内で型を判別するためのキー名。

    /// @brief オブジェクトから型名を取得する内部ヘルパー関数。
    /// @param obj 対象オブジェクト。
    /// @param entries 型名とファクトリ関数のマッピング。
    /// @return 型名。
    /// @throws std::runtime_error マッピングに存在しない型の場合。
    static std::string getTypeNameFromMap(const BaseType& obj, Map entries) {
        // 全エントリを走査し、typeidで一致する型名を検索
        for (const auto& it : entries) {
            auto testObj = it.value();
            if (typeid(obj) == typeid(*testObj)) {
                return std::string(it.key);
            }
        }
        // マッピングに存在しない型の場合
        throw std::runtime_error(std::string("Unknown polymorphic type: ") + typeid(obj).name());
    }

    /// @brief コンテナに要素を追加する。
    /// @param container 追加先のコンテナ。
    /// @param elem 追加する要素。
    static void addElement(ValueType& container, ElementPtrType elem) {
        container.push_back(std::move(elem));
    }

    /// @brief ポリモーフィック要素を書き出す。
    /// @param writer JsonWriterの参照。
    /// @param ptr 書き出す要素。
    /// @param entries 型名とファクトリ関数のマッピング。
    /// @param jsonKey JSON内で型を判別するためのキー名。
    static void writeElement(JsonWriter& writer, const ElementPtrType& ptr,
        Map entries, const char* jsonKey) {
        if (!ptr) {
            writer.null();
            return;
        }
        writer.startObject();
        std::string typeName = getTypeNameFromMap(*ptr, entries);
        writer.key(jsonKey);
        writer.writeObject(typeName);
        auto& fields = ptr->jsonFields();
        fields.writeFieldsOnly(writer, std::to_address(ptr));
        writer.endObject();
    }
};

// ******************************************************************************** トークン種別ディスパッチフィールド

/// @brief JsonTokenTypeの総数。
/// @note この値はJsonTokenType enumの要素数と一致している必要がある。
export inline constexpr std::size_t JsonTokenTypeCount = 12;

/// @brief JSONからの読み取り用エントリ。
/// @tparam ValueType 変換対象の値型。
export template <typename ValueType>
struct FromJsonEntry {
    JsonTokenType tokenType;                          ///< 対象のトークン種別。
    std::function<ValueType(JsonParser&)> converter;  ///< 読み取り関数。
};

/// @brief JSONトークン種別に応じて変換処理を切り替えるフィールド。
/// @tparam MemberPtrType メンバー変数へのポインタ型。
/// @details fromJsonでは次のトークン種別に対応するコンバータを使用して値を読み取る。
///          toJsonでは指定されたコンバータを使用して書き出す。
export template <typename MemberPtrType>
struct JsonTokenDispatchField : JsonField<MemberPtrType> {
    using Base = JsonField<MemberPtrType>;
    using typename Base::ValueType;
    using ToConverter = std::function<void(JsonWriter&, const ValueType&)>;

    /// @brief コンストラクタ。
    /// @param memberPtr メンバーポインタ。
    /// @param keyName JSONキー名。
    /// @param fromEntries トークン種別順に並んだ読み取りエントリ配列（要素数はJsonTokenTypeCount以下）。
    /// @param toConverter 書き出し用コンバータ関数。
    /// @param req 必須フィールドかどうか。
    template <std::size_t FromN>
    explicit JsonTokenDispatchField(MemberPtrType memberPtr, const char* keyName,
        const std::array<FromJsonEntry<ValueType>, FromN>& fromEntries,
        ToConverter toConverter,
        bool req = false)
        : Base(memberPtr, keyName, req), toConverter_(std::move(toConverter)) {
        static_assert(FromN <= JsonTokenTypeCount);
        // まず全要素を例外を投げる関数で初期化
        for (std::size_t i = 0; i < JsonTokenTypeCount; ++i) {
            fromEntries_[i] = [](JsonParser&) -> ValueType {
                throw std::runtime_error("No converter found for token type");
            };
        }
        // fromEntriesの各エントリをtokenTypeに基づいて設定
        for (std::size_t i = 0; i < FromN; ++i) {
            std::size_t index = static_cast<std::size_t>(fromEntries[i].tokenType);
            fromEntries_[index] = fromEntries[i].converter;
        }
    }

    /// @brief JSONから値を読み取る。
    /// @param parser JsonParserの参照。
    /// @return 読み取った値。
    /// @throws std::runtime_error 対応するコンバータが見つからない場合。
    ValueType fromJson(JsonParser& parser) const {
        // トークン種別をインデックスとして直接アクセス
        std::size_t index = static_cast<std::size_t>(parser.nextTokenType());
        return fromEntries_[index](parser);
    }

    /// @brief JSONに値を書き出す。
    /// @param writer JsonWriterの参照。
    /// @param value 書き出す値。
    void toJson(JsonWriter& writer, const ValueType& value) const {
        toConverter_(writer, value);
    }

private:
    std::array<std::function<ValueType(JsonParser&)>, JsonTokenTypeCount> fromEntries_{}; ///< トークン種別をインデックスとする読み取り関数配列。
    ToConverter toConverter_; ///< 書き出し用コンバータ関数。
};

}  // namespace rai::json
