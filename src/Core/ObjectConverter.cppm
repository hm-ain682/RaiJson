/// @file ObjectConverter.cppm
/// @brief JSONの値変換コンバータ群を提供する。

module;
#include <memory>
#include <concepts>
#include <type_traits>
#include <utility>
#include <string>
#include <string_view>
#include <stdexcept>
#include <array>
#include <optional>
#include <variant>
#include <functional>
#include <ranges>
#include <span>

export module rai.serialization.core:object_converter;

import :format_io;
import :object_serializer;
import rai.collection.sorted_hash_array_map;
import rai.serialization.token_manager;
import rai.serialization.json;

export namespace rai::serialization {

// ******************************************************************************** concept

/// @brief フォーマットへの書き出しと読み込みを行うコンバータに要求される条件を定義する concept。
/// @tparam Converter コンバータ型
/// @tparam Value コンバータが扱う値の型
template <typename Converter, typename Value>
concept IsObjectConverter = std::is_class_v<Converter>
    && requires { typename Converter::Value; }
    && std::is_same_v<typename Converter::Value, Value>
    && requires(const Converter& converter, FormatWriter& writer, const Value& value) {
        converter.write(writer, value);
    }
    && requires(const Converter& converter, FormatReader& parser) {
        { converter.read(parser) } -> std::same_as<Value>;
    };

/// @brief readメソッドを持つ型を表すconcept。
/// @tparam T 型。
template <typename T>
concept HasReadFormatCore = requires(T& obj, FormatReader& parser) {
    { obj.read(parser) } -> std::same_as<void>;
};

/// @brief writeメソッドを持つ型を表すconcept。
/// @tparam T 型。
template <typename T>
concept HasWriteFormatCore = requires(const T& obj, FormatWriter& writer) {
    { obj.write(writer) } -> std::same_as<void>;
};

/// @brief std::optional 型かどうかを判定する trait。
/// @tparam T 判定対象の型。
template <typename T>
struct IsStdOptionalCore : std::false_type {};

/// @brief std::optional 型かどうかを判定する trait の特殊化。
/// @tparam T optional の要素型。
template <typename T>
struct IsStdOptionalCore<std::optional<T>> : std::true_type {};

/// @brief std::optional 型かどうかを判定する concept。
/// @tparam T 判定対象の型。
template <typename T>
concept IsStdOptional = IsStdOptionalCore<std::remove_cvref_t<T>>::value;

/// @brief 型 `T` に応じた既定のコンバータを返すユーティリティの前方宣言。
/// @tparam T 変換対象型。
/// @return 型 `T` に対応する既定コンバータへの参照。
template <typename T>
constexpr auto& getConverter();

// ******************************************************************************** 基本型用変換方法

/// @brief プリミティブ型（int, double, bool など）かどうかを判定するconcept。
/// @tparam T 判定対象の型。
template <typename T>
concept IsFundamentalValue = std::is_fundamental_v<T>;

/// @brief プリミティブ型、文字列型の変換方法。
template <typename T>
struct FundamentalConverter {
    static_assert(IsFundamentalValue<T> || std::same_as<T, std::string>,
        "FundamentalConverter requires T to be a fundamental JSON value or std::string");
    using Value = T;

    void write(JsonWriter& writer, const T& value) const {
        writer.writeObject(value);
    }

    T read(JsonParser& parser) const {
        T out{};
        parser.readTo(out);
        return out;
    }
};

/// @brief 指定されたObjectSerializer派生クラスによる任意型の変換方法。
/// @tparam T 変換対象型
/// @tparam Serializer ObjectSerializer派生クラス
template <typename T, typename Serializer>
struct ObjectSerializerConverter {
    using Value = T;

    explicit ObjectSerializerConverter(const Serializer& serializer)
        : serializer_(serializer) {}

    void write(JsonWriter& writer, const Value& value) const {
        writer.startObject();
        serializer_.writeFields(writer, static_cast<const void*>(&value));
        writer.endObject();
    }

    Value read(JsonParser& parser) const {
        Value out{};
        parser.startObject();
        serializer_.readFields(parser, static_cast<void*>(&out));
        parser.endObject();
        return out;
    }

private:
    const Serializer& serializer_;
};

template <typename T, typename Serializer>
constexpr auto makeObjectSerializerConverter(const Serializer& serializer) {
    return ObjectSerializerConverter<T, Serializer>{serializer};
}

/// @brief serializer()メンバー関数を持つかどうかを判定するconcept。
/// @tparam T 判定対象の型。
template <typename T>
concept HasSerializer = requires(const T& t) { t.serializer(); };

/// @brief serializer() を直接使う型のコンバータ。
template <typename T>
struct SerializerConverter {
    static_assert(HasSerializer<T> && std::default_initializable<T>,
        "SerializerConverter requires T to have serializer() and be default-initializable");
    using Value = T;

    void write(FormatWriter& writer, const T& obj) const {
        writer.startObject();
        const auto& fields = obj.serializer();
        fields.writeFields(writer, static_cast<const void*>(&obj));
        writer.endObject();
    }

    T read(FormatReader& parser) const {
        T obj{};
        parser.startObject();
        auto& fields = obj.serializer();
        fields.readFields(parser, static_cast<void*>(&obj));
        parser.endObject();
        return obj;
    }
};

/// @brief readメソッドを持つ型を表すconcept。
/// @tparam T 型。
template <typename T>
concept HasReadFormat = HasReadFormatCore<T>;

/// @brief writeメソッドを持つ型を表すconcept。
/// @tparam T 型。
template <typename T>
concept HasWriteFormat = HasWriteFormatCore<T>;

/// @brief write/readを持つ型のコンバータ
template <typename T>
struct ReadWriteFormatConverter {
    static_assert(HasReadFormat<T> && HasWriteFormat<T> && std::default_initializable<T>,
        "ReadWriteFormatConverter requires T to have read/write and be default-initializable");
    using Value = T;

    void write(FormatWriter& writer, const T& obj) const {
        obj.write(writer);
    }
    T read(FormatReader& parser) const {
        T out{};
        out.read(parser);
        return out;
    }
};

// ******************************************************************************** optional用変換方法

/// @brief std::optional 用のコンバータ。
/// @tparam T std::optional 型。
/// @tparam ElementConverter optional の要素型に対するコンバータ。
template <typename T, typename ElementConverter>
struct OptionalConverter {
    static_assert(IsStdOptional<T>, "OptionalConverter requires T to be std::optional<U>");

    using Value = T;
    using Element = typename T::value_type;
    using ElementConverterType = std::remove_cvref_t<ElementConverter>;
    static_assert(IsObjectConverter<ElementConverterType, Element>,
        "OptionalConverter requires ElementConverter to satisfy IsObjectConverter");

    /// @brief 要素コンバータを指定して構築する。
    /// @param converter optional の要素型に対するコンバータ。
    constexpr explicit OptionalConverter(const ElementConverter& converter)
        : elementConverter_(std::cref(converter)) {}

    /// @brief optional値をJSONへ書き出す。
    /// @param writer 出力先ライタ。
    /// @param value 書き出す optional 値。
    void write(JsonWriter& writer, const T& value) const {
        if (!value.has_value()) {
            writer.null();
            return;
        }
        elementConverter_.get().write(writer, *value);
    }

    /// @brief JSONからoptional値を読み込む。
    /// @param parser 入力元パーサ。
    /// @return 読み込んだ optional 値。
    T read(JsonParser& parser) const {
        if (parser.nextIsNull()) {
            parser.skipValue();
            return std::nullopt;
        }
        Element element = elementConverter_.get().read(parser);
        return T{ std::move(element) };
    }

private:
    /// @brief optional の要素型に対するコンバータ参照。
    std::reference_wrapper<const ElementConverterType> elementConverter_{};
};

/// @brief std::optional 型に対応する既定の `OptionalConverter` を作成する。
/// @tparam T std::optional 型。
/// @return `T` に対応する既定の OptionalConverter 参照。
template <typename T>
constexpr auto& getOptionalConverter() {
    static_assert(IsStdOptional<T>, "getOptionalConverter requires T to be std::optional<U>");

    using Element = typename T::value_type;
    const auto& elementConverter = getConverter<Element>();
    using ElementConverterType = std::remove_cvref_t<decltype(elementConverter)>;
    static const OptionalConverter<T, ElementConverterType> instance{ elementConverter };
    return instance;
}

/// @brief 要素コンバータを指定して `OptionalConverter` を作成する。
/// @tparam T std::optional 型。
/// @tparam ElementConverter 要素コンバータ型。
/// @param elementConverter optional の要素型に対するコンバータ。
/// @return 要素コンバータ指定済みの OptionalConverter。
template <typename T, typename ElementConverter>
constexpr auto getOptionalConverter(const ElementConverter& elementConverter) {
    return OptionalConverter<T, ElementConverter>(elementConverter);
}

/// @brief 標準でサポートする型を判定する。
/// @tparam T 判定対象の型
template <typename T>
concept IsDefaultConverterSupported
    = IsFundamentalValue<T>
    || std::same_as<T, std::string>
    || IsStdOptional<T>
    || HasSerializer<T>
    || (HasReadFormat<T> && HasWriteFormat<T>);

/// @brief 型 `T` に応じた既定のコンバータを返すユーティリティ。
/// @note 基本型、`HasSerializer`、`HasReadFormat`/`HasWriteFormat` を持つ型を自動的に扱い、その他の複雑な型は明確な static_assert で除外します。
template <typename T>
constexpr auto& getConverter() {
    if constexpr (IsFundamentalValue<T> || std::same_as<T, std::string>) {
        static const FundamentalConverter<T> inst{};
        return inst;
    }
    else if constexpr (IsStdOptional<T>) {
        return getOptionalConverter<T>();
    }
    else if constexpr (HasSerializer<T>) {
        static const SerializerConverter<T> inst{};
        return inst;
    }
    else if constexpr (HasReadFormat<T> && HasWriteFormat<T>) {
        static const ReadWriteFormatConverter<T> inst{};
        return inst;
    }
    else {
        static_assert(false,
            "getConverter: unsupported type");
    }
}

// ******************************************************************************** enum用変換方法

// EnumTextMapのように、enum <-> 文字列名の双方向マップを提供する型のconcept。
template <typename Map>
concept IsEnumTextMap
    = requires { typename Map::Enum; }
    && std::is_enum_v<typename Map::Enum>
    && requires(const Map& m, std::string_view s, typename Map::Enum v) {
        { m.fromName(s) } -> std::same_as<std::optional<typename Map::Enum>>;
        { m.toName(v) } -> std::same_as<std::optional<std::string_view>>;
    };

/// @brief EnumEntry は enum 値と文字列名の対応を保持します
template <typename EnumType>
struct EnumEntry {
    EnumType value;   ///< Enum値。
    const char* name; ///< 対応する文字列名。
};

/// @brief EnumEntry を利用して enum <-> name の双方向マップを持つ再利用可能な型。
/// @tparam EnumType enum 型
/// @tparam N エントリ数（静的）
template <typename EnumType, std::size_t N>
struct EnumTextMap {
    using Enum = EnumType;

    /// @brief std::span ベースのコンストラクタ（C配列やstd::arrayからの変換を受け取ります）
    constexpr explicit EnumTextMap(std::span<const EnumEntry<Enum>> entries) {
        if (entries.size() != N) {
            throw std::runtime_error("EnumTextMap(span): size must match template parameter N");
        }
        std::pair<std::string_view, Enum> nv[N];
        for (std::size_t i = 0; i < N; ++i) {
            nv[i] = { entries[i].name, entries[i].value };
        }
        nameToValue_ = collection::SortedHashArrayMap<std::string_view, Enum, N>(nv);

        std::pair<Enum, std::string_view> vn[N];
        for (std::size_t i = 0; i < N; ++i) {
            vn[i] = { entries[i].value, entries[i].name };
        }
        valueToName_ = collection::SortedHashArrayMap<Enum, std::string_view, N>(vn);
    }

    /// @brief 文字列から enum を得る。見つからない場合は nullopt。
    constexpr std::optional<Enum> fromName(std::string_view name) const {
        if (auto p = nameToValue_.findValue(name)) {
            return *p;
        }
        return std::nullopt;
    }

    /// @brief enum から文字列名を得る。見つからない場合は nullopt。
    constexpr std::optional<std::string_view> toName(Enum v) const {
        if (auto p = valueToName_.findValue(v)) {
            return *p;
        }
        return std::nullopt;
    }

private:
    ///! 名前からenum値へのマップ。
    collection::SortedHashArrayMap<std::string_view, Enum, N> nameToValue_{};
    ///! enum値から名前へのマップ。
    collection::SortedHashArrayMap<Enum, std::string_view, N> valueToName_{};
};

/// @brief 列挙型用のコンバータ
/// @tparam MapType EnumTextMap型など
template <typename MapType>
struct EnumConverter {
    static_assert(IsEnumTextMap<MapType>,
        "EnumConverter requires MapType to satisfy IsEnumTextMap");
    using Enum = typename MapType::Enum;
    using Value = Enum;
    
    constexpr explicit EnumConverter(const MapType& map)
        : map_(map) {}

    void write(JsonWriter& writer, const Enum& value) const {
        if (auto name = map_.toName(value)) {
            writer.writeObject(*name);
            return;
        }
        throw std::runtime_error("Failed to convert enum to string");
    }

    Enum read(JsonParser& parser) const {
        std::string jsonValue;
        parser.readTo(jsonValue);
        if (auto v = map_.fromName(jsonValue)) {
            return *v;
        }
        throw std::runtime_error(std::string("Failed to convert string to enum: ") + jsonValue);
    }
private:
    MapType map_;
};

/// @brief C 配列から EnumConverter を構築する。
template <typename Enum, std::size_t N>
constexpr auto getEnumConverter(const EnumEntry<Enum> (&entries)[N]) {
    const EnumTextMap<Enum, N> map{ std::span<const EnumEntry<Enum>, N>(entries) };
    return EnumConverter<EnumTextMap<Enum, N>>(map);
}

/// @brief array から EnumConverter を構築する。
template <typename Enum, std::size_t M>
constexpr auto getEnumConverter(const std::array<EnumEntry<Enum>, M>& entries) {
    const EnumTextMap<Enum, M> map{ std::span<const EnumEntry<Enum>, M>(entries.data(), M) };
    return EnumConverter<EnumTextMap<Enum, M>>(map);
}

/// @brief spanから EnumConverter を構築する。
template <typename Enum, std::size_t N>
constexpr auto getEnumConverter(std::span<const EnumEntry<Enum>, N> entries) {
    const EnumTextMap<Enum, N> map{ entries };
    return EnumConverter<EnumTextMap<Enum, N>>(map);
}

// ******************************************************************************** コンテナ用変換方法

/// @brief 文字列系型かどうかを判定するconcept。
/// @tparam T 判定対象の型。
template <typename T>
concept LikesString = std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>;

/// @brief string 系を除くレンジ（配列/コンテナ）を表す concept。
/// @details std::ranges::range を満たし、かつ `LikesString` を除外することで
///          `std::string` を配列として誤判定しないようにします。
/// @tparam T 判定対象の型。
template<typename T>
concept IsContainer = std::ranges::range<T> && !LikesString<T>;

/// @brief コンテナに要素を追加するヘルパー。
/// @tparam Container 追加先コンテナ。
/// @tparam Element 追加する要素の型。
/// @param container 追加先コンテナ。
/// @param element 追加する要素。
template <typename Container, typename Element>
constexpr void insertContainerElement(Container& container, Element&& element) {
    if constexpr (requires(Container& c, Element&& v) {
            c.push_back(std::forward<Element>(v));
        }) {
        container.push_back(std::forward<Element>(element));
    }
    else if constexpr (requires(Container& c, Element&& v) {
            c.insert(std::forward<Element>(v));
        }) {
        container.insert(std::forward<Element>(element));
    }
    else {
        static_assert(false,
            "insertContainerElement: container must support push_back or insert");
    }
}

/// @brief コンテナの変換方法。
template <typename Container, typename ElementConverter>
struct ContainerConverter {
    static_assert(IsContainer<Container>,
        "ContainerConverter requires Container to be a container type");
    using Value = Container;
    using Element = std::remove_cvref_t<std::ranges::range_value_t<Container>>;
    static_assert(IsObjectConverter<ElementConverter, Element>,
        "ElementConverter must satisfy IsObjectConverter for container element type");
    using ElementConverterT = std::remove_cvref_t<ElementConverter>;
    static_assert(std::is_same_v<typename ElementConverterT::Value, Element>,
        "ElementConverter::Value must match container element type");

    constexpr explicit ContainerConverter(const ElementConverter& elemConv)
        : elementConverter_(std::cref(elemConv)) {}

    void write(JsonWriter& writer, const Container& range) const {
        writer.startArray();
        for (const auto& e : range) {
            elementConverter_.get().write(writer, e);
        }
        writer.endArray();
    }

    Container read(JsonParser& parser) const {
        Container out{};
        parser.startArray();
        while (!parser.nextIsEndArray()) {
            auto elem = elementConverter_.get().read(parser);
            insertContainerElement(out, std::move(elem));
        }
        parser.endArray();
        return out;
    }

private:
    std::reference_wrapper<const ElementConverterT> elementConverter_{};
};

/// @brief コンテナ型に対応する既定の `ContainerConverter` を作成する。
/// @tparam Container コンテナ型
template <typename Container>
constexpr auto getContainerConverter() {
    using Elem = std::remove_cvref_t<std::ranges::range_value_t<Container>>;
    const auto& elementConverter = getConverter<Elem>();
    using ElemConv = std::remove_cvref_t<decltype(elementConverter)>;
    return ContainerConverter<Container, ElemConv>(elementConverter);
}

/// @brief 明示的な要素コンバータから `ContainerConverter` を作成する。
/// @tparam Container コンテナ型
/// @tparam ElementConverter 要素コンバータ型
/// @param elemConv 要素コンバータ
template <typename Container, typename ElementConverter>
constexpr auto getContainerConverter(const ElementConverter& elemConv) {
    return ContainerConverter<Container, ElementConverter>(elemConv);
}

// ******************************************************************************** 二重配列表形式

template <typename Serializer, typename Container>
concept IsFieldsObjectSerializer = requires(const Serializer& s,
        std::remove_cvref_t<std::ranges::range_value_t<Container>>& item) {
        { s.size() } -> std::same_as<std::size_t>;
        { s.getFieldName(std::size_t{}) } -> std::same_as<std::string_view>;
        { s.writeFieldAt(std::size_t{}, std::declval<FormatWriter&>(),
            std::declval<const std::remove_cvref_t<std::ranges::range_value_t<Container>>&>()) };
        { s.readFieldAt(std::size_t{}, std::declval<FormatReader&>(), item) };
        { s.applyMissingAt(std::size_t{}, item) };
    };

/// @brief 二重配列での表形式（カラム名配列＋値配列）でJSON変換するConverter。
/// @tparam Container コンテナ型。
/// @tparam Serializer フィールドシリアライザ型。
template <typename Container, typename Serializer>
class ColumnarConverter {
public:
    using Value = Container;
    using Element = std::remove_cvref_t<std::ranges::range_value_t<Container>>;

    static_assert(std::is_same_v<typename Serializer::Owner, Element>,
        "Serializer::Owner must match container element type");
    static_assert(IsFieldsObjectSerializer<Serializer, Container>,
        "Serializer must satisfy IsFieldsObjectSerializer for the container type");

    explicit ColumnarConverter(const Serializer& serializer)
        : serializer_(serializer) {}

    /// @brief 配列をカラム名リスト＋値リスト形式でJSON出力。
    void write(FormatWriter& writer, const Value& value) const {
        writer.startArray();
        // カラム名リスト
        writer.startArray();
        for (std::size_t i = 0; i < serializer_.size(); ++i) {
            writer.writeObject(serializer_.getFieldName(i));
        }
        writer.endArray();
        // 各行データ
        for (const auto& item : value) {
            writer.startArray();
            for (std::size_t i = 0; i < serializer_.size(); ++i) {
                serializer_.writeFieldAt(i, writer, item);
            }
            writer.endArray();
        }
        writer.endArray();
    }

    /// @brief JSON配列から配列を復元。
    Value read(FormatReader& parser) const {
        parser.startArray();
        // カラム名リスト
        parser.startArray();
        const std::size_t fieldCount = serializer_.size();
        std::vector<std::size_t> fieldOrder;
        fieldOrder.reserve(fieldCount);
        std::vector<bool> seen(fieldCount, false);
        while (!parser.nextIsEndArray()) {
            std::string name;
            parser.readTo(name);
            const std::size_t fieldIndex = serializer_.getFieldIndex(name);
            if (seen[fieldIndex]) {
                throw std::runtime_error(std::string("ColumnarConverter: duplicate column name '") + name + "'");
            }
            seen[fieldIndex] = true;
            fieldOrder.push_back(fieldIndex);
        }
        parser.endArray();

        // 行ごとに無駄な反復を繰り返さないように、欠落フィールドのインデックスを事前に収集しておく。
        std::vector<std::size_t> missingFields;
        missingFields.reserve(fieldCount);
        for (std::size_t index = 0; index < fieldCount; ++index) {
            if (!seen[index]) {
                missingFields.push_back(index);
            }
        }

        // 各行データ
        Value out{};
        while (!parser.nextIsEndArray()) {
            parser.startArray();

            // 1行分のデータを読み込む。
            Element item{};
            std::size_t readCount = 0;
            for (std::size_t fieldIndex : fieldOrder) {
                if (parser.nextIsEndArray()) {
                    break;
                }
                serializer_.readFieldAt(fieldIndex, parser, item);
                ++readCount;
            }
            while (!parser.nextIsEndArray()) {
                parser.skipValue();
            }
            parser.endArray();

            // ヘッダーに存在するがこの行にデータがないフィールドは欠落とする。
            for (std::size_t i = readCount; i < fieldOrder.size(); ++i) {
                serializer_.applyMissingAt(fieldOrder[i], item);
            }

            // ヘッダーに存在しないフィールドは常に欠落とする。
            for (std::size_t missingIndex : missingFields) {
                serializer_.applyMissingAt(missingIndex, item);
            }
            insertContainerElement(out, std::move(item));
        }
        parser.endArray();
        return out;
    }

private:
    const Serializer& serializer_;
};

/// @brief 要素の変換方法を指定して ColumnarConverter<std::vector<T>, Serializer> を返す。
/// @tparam T 配列要素型
/// @tparam Serializer ObjectSerializer派生型
template <typename T, typename Serializer>
requires (!IsContainer<T>)
constexpr auto getColumnarConverter(const Serializer& serializer) {
    return ColumnarConverter<std::vector<T>, Serializer>{serializer};
}

/// @brief コンテナ型を指定して ColumnarConverter を返す。
/// @tparam Container コンテナ型
/// @tparam Serializer ObjectSerializer派生型
template <typename Container, typename Serializer>
    requires IsContainer<Container>
constexpr auto getColumnarConverter(const Serializer& serializer) {
    return ColumnarConverter<Container, Serializer>{serializer};
}

// ******************************************************************************** unique_ptr用変換方法

/// @brief std::unique_ptr を判定する concept（element_type / deleter_type を確認し正確に判定）。
/// @tparam T 判定対象の型。
template <typename T>
concept IsUniquePtr = requires {
    typename T::element_type;
    typename T::deleter_type;
} && std::is_same_v<T, std::unique_ptr<typename T::element_type, typename T::deleter_type>>;

/// @brief unique_ptr 等のコンバータ
template <typename T, typename TargetConverter>
struct UniquePtrConverter {
    using Value = T;
    using Element = typename T::element_type;
    using ElemConvT = std::remove_cvref_t<TargetConverter>;
    static_assert(IsUniquePtr<T>, "UniquePtrConverter requires T to be a unique_ptr-like type");
    static_assert(IsObjectConverter<ElemConvT, Element>,
        "UniquePtrConverter requires ElementConverter to be an ObjectConverter for element type");

    // デフォルト要素コンバータへの参照を返すユーティリティ（静的寿命）
    static const ElemConvT& defaultTargetConverter() {
        static const ElemConvT& inst = getConverter<Element>();
        return inst;
    }

    // デフォルトコンストラクタはデフォルト要素コンバータへの参照を初期化子リストで設定する
    UniquePtrConverter()
        : targetConverter_(std::cref(defaultTargetConverter())) {}

    // 明示的に要素コンバータ参照を指定するオーバーロード
    constexpr explicit UniquePtrConverter(const ElemConvT& conv)
        : targetConverter_(std::cref(conv)) {}

    void write(JsonWriter& writer, const T& ptr) const {
        if (!ptr) {
            writer.null();
            return;
        }
        targetConverter_.get().write(writer, *ptr);
    }

    T read(JsonParser& parser) const {
        if (parser.nextIsNull()) {
            parser.skipValue();
            return nullptr;
        }
        auto elem = targetConverter_.get().read(parser);
        return std::make_unique<Element>(std::move(elem));
    }

private:
    std::reference_wrapper<const ElemConvT> targetConverter_;
};

/// @brief unique_ptr<T>のjson変換方法を返す。※インスタンスはstatic。
/// @tparam T 参照先がgetConverterの対象であるunique_ptr型
template <typename T>
constexpr auto& getUniquePtrConverter() {
    using TargetConverter = decltype(getConverter<typename T::element_type>());
    static const UniquePtrConverter<T, TargetConverter> inst{};
    return inst;
}

/// @brief 参照先の変換方法を指定して unique_ptr<T> のjson変換方法を返す。
/// @tparam T unique_ptr型
/// @param elementConverter 参照先の変換方法
template <typename T, typename ElementConverter>
constexpr auto getUniquePtrConverter(const ElementConverter& elementConverter) {
    return UniquePtrConverter<T, ElementConverter>(elementConverter);
}

// ******************************************************************************** トークン種別毎の分岐用

/// @brief トークン種別ごとの読み取り／書き出しを提供する基底的な変換方法。
/// @tparam ValueType 値の型
template <typename ValueType>
struct TokenConverter {
    using Value = ValueType;

    // 読み取り（各トークン種別ごとにオーバーライド可能）
    Value readNull(JsonParser& parser) const {
        if constexpr (std::is_constructible_v<Value, std::nullptr_t>) {
            parser.skipValue();
            return Value(nullptr);
        }
        else {
            throw std::runtime_error("Null is not supported for TokenConverter");
        }
    }

    Value readBool(JsonParser& parser) const {
        return this->template read<bool>(parser, "Bool is not supported for TokenConverter");
    }

    Value readInteger(JsonParser& parser) const {
        return this->template read<int>(parser, "Integer is not supported for TokenConverter");
    }

    Value readNumber(JsonParser& parser) const {
        return this->template read<double>(parser, "Number is not supported for TokenConverter");
    }

    Value readString(JsonParser& parser) const {
        return this->template read<std::string>(parser, "String is not supported for TokenConverter");
    }

    Value readStartObject(JsonParser& parser) const {
        if constexpr (HasSerializer<Value> || (HasReadFormat<Value> && HasWriteFormat<Value>)) {
            return getConverter<Value>().read(parser);
        }
        else {
            throw std::runtime_error("Object is not supported for TokenConverter");
        }
    }

    Value readStartArray(JsonParser& parser) const {
        // デフォルトでは配列はサポートしない（必要なら派生で実装）
        throw std::runtime_error("Array is not supported for TokenConverter");
    }
protected:
    template <typename T>
    static constexpr Value read(JsonParser& parser, const char* errorMessage) {
        if constexpr (std::is_constructible_v<Value, T>) {
            T s;
            parser.readTo(s);
            return Value(s);
        }
        else {
            throw std::runtime_error(errorMessage);
        }
    }

    void write(JsonWriter& writer, const Value& value) const {
        if constexpr (IsDefaultConverterSupported<Value>) {
            getConverter<Value>().write(writer, value);
        }
        else {
            static_assert(false, "TokenConverter::write: unsupported Value type");
        }
    }
};

/// @brief トークン種別に応じた変換方法
template <typename ValueType, typename TokenConv = TokenConverter<ValueType>>
struct TokenDispatchConverter {
    using Value = ValueType;
    using TokenConvT = std::remove_cvref_t<TokenConv>;
    static_assert(std::is_base_of_v<TokenConverter<Value>, TokenConvT>,
        "TokenConv must be TokenConverter<Value> or derived from it");

    // コンストラクタ（TokenConverter を受け取る）
    constexpr explicit TokenDispatchConverter(const TokenConvT& conv = TokenConvT())
        : tokenConverter_(conv) {}

    /// @brief トークン種別に応じて適切な変換関数を呼び出して値を読み取る。
    ValueType read(JsonParser& parser) const {
        switch (parser.nextTokenType()) {
        case JsonTokenType::Null:        return tokenConverter_.readNull(parser);
        case JsonTokenType::Bool:        return tokenConverter_.readBool(parser);
        case JsonTokenType::Integer:     return tokenConverter_.readInteger(parser);
        case JsonTokenType::Number:      return tokenConverter_.readNumber(parser);
        case JsonTokenType::String:      return tokenConverter_.readString(parser);
        case JsonTokenType::StartObject: return tokenConverter_.readStartObject(parser);
        case JsonTokenType::StartArray:  return tokenConverter_.readStartArray(parser);
        default: throw std::runtime_error("Unsupported token type");
        }
    }

    /// @brief 値を JSON に書き出すための関数を呼び出す。
    void write(JsonWriter& writer, const ValueType& value) const {
        tokenConverter_.write(writer, value);
    }

private:
    TokenConvT tokenConverter_{};
};

// ******************************************************************************** variant用変換方法

/// @brief std::variant 型かどうかを判定する concept（std::variant 固有の trait を確認）。
/// @tparam T 判定対象の型。
template <typename T>
concept IsStdVariant = requires {
    typename std::variant_size<T>::type;
};

/// @brief std::variant の要素ごとの変換方法。独自型を扱う場合はこれを継承してカスタマイズする。
/// @tparam Variant std::variant 型
template <typename Variant>
struct VariantElementConverter : TokenConverter<Variant> {
    static_assert(IsStdVariant<Variant>,
        "VariantElementConverter requires Variant to be a std::variant");

    /// @brief Null トークンを読み取り、variant を返す。
    /// @param parser 読み取り元の JsonParser
    /// @return 読み取った値
    Variant readNull(JsonParser& parser) const {
        (void)parser;
        if constexpr (canAssignNullptr()) {
            return Variant{ nullptr };
        }
        throw std::runtime_error("Null is not supported in variant");
    }

    /// @brief Bool トークンを読み取り、variant を返す。
    /// @param parser 読み取り元の JsonParser
    /// @return 読み取った値
    Variant readBool(JsonParser& parser) const {
        if constexpr (canAssign<bool>()) {
            bool value{};
            parser.readTo(value);
            return Variant{ value };
        }
        throw std::runtime_error("Bool is not supported in variant");
    }

    /// @brief Integer トークンを読み取り、variant を返す。
    /// @param parser 読み取り元の JsonParser
    /// @return 読み取った値
    Variant readInteger(JsonParser& parser) const {
        if constexpr (canAssign<int>()) {
            int value{};
            parser.readTo(value);
            return Variant{ value };
        }
        throw std::runtime_error("Integer is not supported in variant");
    }

    /// @brief Number トークンを読み取り、variant を返す。
    /// @param parser 読み取り元の JsonParser
    /// @return 読み取った値
    Variant readNumber(JsonParser& parser) const {
        if constexpr (canAssign<double>()) {
            double value{};
            parser.readTo(value);
            return Variant{ value };
        }
        throw std::runtime_error("Number is not supported in variant");
    }

    /// @brief String トークンを読み取り、variant を返す。
    /// @param parser 読み取り元の JsonParser
    /// @return 読み取った値
    Variant readString(JsonParser& parser) const {
        if constexpr (canAssign<std::string>()) {
            std::string value{};
            parser.readTo(value);
            return Variant{ std::move(value) };
        }
        throw std::runtime_error("String is not supported in variant");
    }

    /// @brief StartArray トークンを読み取り、variant を返す。
    /// @param parser 読み取り元の JsonParser
    /// @return 読み取った値
    Variant readStartArray(JsonParser& parser) const {
        (void)parser;
        throw std::runtime_error("Array is not supported in variant");
    }

    /// @brief StartObject トークンを読み取り、variant を返す。
    /// @param parser 読み取り元の JsonParser
    /// @return 読み取った値
    Variant readStartObject(JsonParser& parser) const {
        bool found = false;
        Variant out{};
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            // Evaluate alternatives in order; stop at the first that matches
            ((void)(!found && ([&]() {
                using Alt = std::remove_cvref_t<typename std::variant_alternative_t<I, Variant>>;
                if constexpr (HasSerializer<Alt> || (HasReadFormat<Alt> && HasWriteFormat<Alt>)) {
                    out = getConverter<Alt>().read(parser);
                    found = true;
                }
                return 0;
            }())), ...);
        }(std::make_index_sequence<std::variant_size_v<Variant>>{});

        if (!found) {
            throw std::runtime_error("Object is not supported in variant");
        }
        return out;
    }

    /// @brief Variant 値を JSON に書き出す。
    /// @param writer 書き込み先の JsonWriter
    /// @param value 書き込む値
    void write(
        JsonWriter& writer, const Variant& value) const {
        std::visit([&](const auto& inner) {
            this->write(writer, inner);
        }, value);
    }

    template<typename T>
    void write(JsonWriter& writer, const T& value) const {
        static const auto& conv = getConverter<std::remove_cvref_t<T>>();
        conv.write(writer, value);
    }
private:
    static constexpr bool canAssignNullptr() noexcept {
        using Null = std::nullptr_t;
        return []<size_t... I>(std::index_sequence<I...>) {
            return (std::is_assignable_v<
                typename std::variant_alternative_t<I, Variant>&,
                Null
            > || ...);
        }(std::make_index_sequence<std::variant_size_v<Variant>>{});
    }
private:
    template<class T>
    static constexpr bool canAssign() noexcept {
        using U = std::remove_cvref_t<T>;
        return []<size_t... I>(std::index_sequence<I...>) {
            return (std::is_same_v<U, std::remove_cvref_t<
                typename std::variant_alternative_t<I, Variant>>>
                || ...);
        }(std::make_index_sequence<std::variant_size_v<Variant>>{});
    }
};

/// @brief Variant 用の TokenDispatchConverter を構築するヘルパー（既定の要素変換器）。
template <typename Variant>
constexpr auto getVariantConverter() {
    static_assert(IsStdVariant<Variant>,
        "getVariantConverter requires Variant to be a std::variant");
    using ElementConverter = VariantElementConverter<Variant>;
    return TokenDispatchConverter<Variant, ElementConverter>(ElementConverter{});
}

/// @brief Variant 用の TokenDispatchConverter を構築するヘルパー（要素変換器指定）。
template <typename Variant, typename ElementConverterType>
constexpr auto getVariantConverter(ElementConverterType elementConverter) {
    static_assert(IsStdVariant<Variant>,
        "getVariantConverter requires Variant to be a std::variant");
    static_assert(std::is_base_of_v<VariantElementConverter<Variant>,
        std::remove_cvref_t<ElementConverterType>>,
        "ElementConverterType must be derived from VariantElementConverter<Variant>");
    using ElementConverter = std::remove_cvref_t<ElementConverterType>;
    return TokenDispatchConverter<Variant, ElementConverter>(
        ElementConverter(std::move(elementConverter)));
}

// ******************************************************************************** 汎用的な読み書き関数

/// @brief valueをconverterで変換してwriterに書き出す。
/// @tparam Converter 
/// @tparam Value 
/// @param writer 
/// @param converter 
/// @param value 
template <typename Converter, typename Value>
    requires IsObjectConverter<Converter, Value>
void write(JsonWriter& writer, const Converter& converter, const Value& value) {
    converter.write(writer, value);
}

/// @brief parserから読み込んでconverterでValueに変換して返す。
/// @tparam Converter 
/// @tparam Value 
/// @param parser 
/// @param converter 
/// @return 変換後のValue
template <typename Converter>
    requires IsObjectConverter<Converter, typename Converter::Value>
typename Converter::Value read(JsonParser& parser, const Converter& converter) {
    return converter.read(parser);
}

}  // namespace rai::serialization
