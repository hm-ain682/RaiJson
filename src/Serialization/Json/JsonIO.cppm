// @file JsonIO.cppm
// @brief JSON入出力の統合インターフェース。ObjectSerializerと連携してJSON変換を提供する。

module;
#include <cassert>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <mutex>
#include <future>

export module rai.serialization.json_io;

import rai.serialization.object_converter;
import rai.serialization.object_serializer;
import rai.serialization.format_io;
import rai.serialization.json_writer;
import rai.serialization.json_parser;
import rai.serialization.json_tokenizer;
import rai.serialization.token_manager;
import rai.serialization.reading_ahead_buffer;
import rai.serialization.parallel_input_stream_source;
import rai.common.thread_pool;

namespace rai::serialization {

static constexpr std::size_t smallFileThreshold = 10 * 1024; //< 小ファイルとみなす閾値（byte）
static constexpr std::size_t aheadSize = 8;        //< 先読み8byte

/// @brief 指定型に対してObjectSerializerを提供できるか判定するconcept。
/// @tparam Provider シリアライザー提供者の型。
/// @tparam ObjectType 対象オブジェクト型。
export template <typename Provider, typename ObjectType>
concept IsSerializationProvider = requires(const Provider& provider,
    const ObjectType& constObject, ObjectType& object,
    FormatWriter& writer, FormatReader& reader) {
    {
        provider.getObjectSerializer(constObject)
            .writeFields(writer, static_cast<const void*>(&constObject))
    } -> std::same_as<void>;
    {
        provider.getObjectSerializer(constObject)
            .readFields(reader, static_cast<void*>(&object))
    } -> std::same_as<void>;
};

/// @brief 既定の永続化提供クラス。
///        永続化対象オブジェクトの各型にあるserializer()を利用してObjectSerializerを提供する。
export class SerializerObjectSerializationProvider {
public:
    /// @brief 指定型に対応するObjectSerializerを返す。
    /// @tparam ObjectType 対象オブジェクト型。
    /// @param object 対象オブジェクト。
    /// @return 指定型に対応するObjectSerializer。
    template <HasSerializer ObjectType>
    const auto& getObjectSerializer(const ObjectType& object) const {
        return object.serializer();
    }
};

/// @brief オブジェクトをJSON形式でストリームに書き出す。
/// @tparam T 変換対象の型。
/// @tparam Provider シリアライザー提供者の型。
/// @param obj 変換するオブジェクト。
/// @param os 出力先のストリーム。
/// @param provider 型に対応するObjectSerializer提供者。
export template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void writeJsonToBuffer(const T& obj, std::ostream& os, const Provider& provider) {
    JsonWriter writer(os);
    const auto& objectSerializer = provider.getObjectSerializer(obj);
    writer.startObject();
    objectSerializer.writeFields(writer, static_cast<const void*>(&obj));
    writer.endObject();
}

/// @brief オブジェクトをJSON形式でストリームに書き出す。
/// @tparam T 変換対象の型。
/// @param obj 変換するオブジェクト。
/// @param os 出力先のストリーム。
export template <HasSerializer T>
void writeJsonToBuffer(const T& obj, std::ostream& os) {
    writeJsonToBuffer(obj, os, SerializerObjectSerializationProvider{});
}

/// @brief 任意の型のオブジェクトをJSON形式で文字列化して返す。
/// @tparam T 変換対象の型。
/// @tparam Provider シリアライザー提供者の型。
/// @param obj 変換するオブジェクト。
/// @param provider 型に対応するObjectSerializer提供者。
/// @return JSON形式の文字列。
export template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
std::string getJsonContent(const T& obj, const Provider& provider) {
    std::ostringstream oss;
    writeJsonToBuffer(obj, oss, provider);
    return oss.str();
}

/// @brief 任意の型のオブジェクトをJSON形式で文字列化して返す。
/// @tparam T 変換対象の型。
/// @param obj 変換するオブジェクト。
/// @return JSON形式の文字列。
export template <HasSerializer T>
std::string getJsonContent(const T& obj) {
    return getJsonContent(obj, SerializerObjectSerializationProvider{});
}

/// @brief オブジェクトをJSONファイルに書き出す。
/// @tparam T 変換対象の型。
/// @tparam Provider シリアライザー提供者の型。
/// @param obj 変換するオブジェクト。
/// @param filename 出力先のファイル名。
/// @param provider 型に対応するObjectSerializer提供者。
export template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void writeJsonFile(const T& obj, const std::string& filename, const Provider& provider) {
    std::ofstream ofs(filename, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {
        throw std::runtime_error("writeJsonToFile: Cannot open file " + filename);
    }

    writeJsonToBuffer(obj, ofs, provider);

    ofs.close();
    if (ofs.bad()) {
        throw std::runtime_error("writeJsonToFile: Error writing to file " + filename);
    }
}

/// @brief オブジェクトをJSONファイルに書き出す。
/// @tparam T 変換対象の型。
/// @param obj 変換するオブジェクト。
/// @param filename 出力先のファイル名。
export template <HasSerializer T>
void writeJsonFile(const T& obj, const std::string& filename) {
    writeJsonFile(obj, filename, SerializerObjectSerializationProvider{});
}

/// @brief オブジェクトをJSONから読み込む（startObject/endObject含む）。
/// @tparam T 読み込み対象の型。
/// @tparam Provider シリアライザー提供者の型。
/// @param parser 読み取り元のJsonParser互換オブジェクト。
/// @param obj 読み込み先のオブジェクト。
/// @param provider 型に対応するObjectSerializer提供者。
/// @note トップレベルのJSON読み込み用のヘルパー関数。
export template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void readJsonObject(JsonParser& parser, T& obj, const Provider& provider) {
    const auto& objectSerializer = provider.getObjectSerializer(obj);
    parser.startObject();
    objectSerializer.readFields(parser, static_cast<void*>(&obj));
    parser.endObject();
}

/// @brief オブジェクトをJSONから読み込む（startObject/endObject含む）。
/// @tparam T HasSerializerを実装している型。
/// @param parser 読み取り元のJsonParser互換オブジェクト。
/// @param obj 読み込み先のオブジェクト。
/// @note トップレベルのJSON読み込み用のヘルパー関数。
export template <HasSerializer T>
void readJsonObject(JsonParser& parser, T& obj) {
    readJsonObject(parser, obj, SerializerObjectSerializationProvider{});
}

/// @brief 文字列バッファからオブジェクトを読み込む（コア関数）。
/// @tparam T 読み込み対象の型。
/// @tparam Provider シリアライザー提供者の型。
/// @param buffer 入力バッファ（ReadingAheadBuffer用の先読み領域を含む容量が必要）。
/// @param out 読み込み先のオブジェクト。
/// @param unknownKeysOut 未知キーの収集先。
/// @param jsonFormat オブジェクトのJSON形式を定義するオブジェクト。
template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void readJsonFromBuffer(std::string&& buffer, T& out,
    std::vector<std::string>& unknownKeysOut, const Provider& provider) {
    ReadingAheadBuffer inputSource(std::move(buffer), aheadSize);
    TokenManager tokenManager;
    StdoutMessageOutput warningOutput;
    JsonTokenizer<ReadingAheadBuffer, TokenManager> tokenizer(
        inputSource, tokenManager, warningOutput);
    tokenizer.tokenize();

    JsonParser parser(tokenManager);
    readJsonObject(parser, out, provider);
    unknownKeysOut = std::move(parser.getUnknownKeys());
}

template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void readJsonImpl(std::istream& inputStream, T& out,
    std::vector<std::string>& unknownKeysOut, const Provider& provider) {
    // ストリームから文字列に読み込み
    std::ostringstream oss;
    oss << inputStream.rdbuf();

    // 読み込み失敗をチェック
    if (inputStream.fail() && !inputStream.eof()) {
        throw std::runtime_error("readJsonImpl: Failed to read from input stream");
    }

    std::string buffer = oss.str();
    buffer.reserve(buffer.size() + aheadSize);

    readJsonFromBuffer(std::move(buffer), out, unknownKeysOut, provider);
}

// 未知キーの収集先を受け取るオーバーロード（先に定義）
export template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void readJsonString(const std::string& jsonText, T& out,
    std::vector<std::string>& unknownKeysOut, const Provider& provider) {
    std::istringstream stream(jsonText);
    readJsonImpl(stream, out, unknownKeysOut, provider);
}

// 未知キーの収集先を受け取るオーバーロード（既定フォーマット版）
export template <HasSerializer T>
void readJsonString(const std::string& jsonText, T& out,
    std::vector<std::string>& unknownKeysOut) {
    readJsonString(jsonText, out, unknownKeysOut, SerializerObjectSerializationProvider{});
}

/// @brief JSON文字列からオブジェクトを読み込む。
/// @tparam T 読み込み対象の型。
/// @tparam Provider シリアライザー提供者の型。
/// @param json JSON形式の文字列。
/// @param out 読み込み先のオブジェクト。
/// @param jsonFormat オブジェクトのJSON形式を定義するオブジェクト。
export template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void readJsonString(const std::string& jsonText, T& out, const Provider& provider) {
    std::vector<std::string> unknownKeysOut;
    readJsonString(jsonText, out, unknownKeysOut, provider);
}

/// @brief JSON文字列からオブジェクトを読み込む。
/// @tparam T 読み込み対象の型。
/// @param json JSON形式の文字列。
/// @param out 読み込み先のオブジェクト。
export template <HasSerializer T>
void readJsonString(const std::string& jsonText, T& out) {
    readJsonString(jsonText, out, SerializerObjectSerializationProvider{});
}

/// @brief JSONファイルからオブジェクトを読み込む（逐次処理版、内部実装）。
/// @tparam T 読み込み対象の型。
/// @tparam Provider シリアライザー提供者の型。
/// @param ifs 入力元のファイルストリーム（既にオープン済み）。
/// @param filename エラーメッセージ用のファイル名。
/// @param out 読み込み先のオブジェクト。
/// @param fileSize ファイルサイズ。
/// @param unknownKeysOut 未知キーの収集先。
/// @param jsonFormat オブジェクトのJSON形式を定義するオブジェクト。
template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void readJsonFileSequentialImpl(std::ifstream& ifs, const std::string& filename, T& out,
    std::streamsize fileSize, std::vector<std::string>& unknownKeysOut, const Provider& provider) {
    // どうしてこの実装にしたか：ファイルを一括読み込みしてからトークン化する方が、
    // 小〜中規模ファイルではスレッド同期オーバーヘッドを回避できるため高速
    std::string buffer;
    buffer.reserve(fileSize + aheadSize);
    buffer.resize(fileSize);
    ifs.read(buffer.data(), buffer.capacity());
    auto bytesRead = ifs.gcount();
    assert(bytesRead <= static_cast<std::streamsize>(buffer.size()));
    if (ifs.bad()) {
        throw std::runtime_error("readJsonFileSequential: Error reading from file " + filename);
    }
    buffer.resize(bytesRead);

    readJsonFromBuffer(std::move(buffer), out, unknownKeysOut, provider);
}

/// @brief JSONファイルからオブジェクトを読み込む（逐次処理版）。
/// @tparam T 読み込み対象の型。
/// @tparam Provider シリアライザー提供者の型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
/// @param fileSize ファイルサイズ。
/// @param unknownKeysOut 未知キーの収集先。
/// @param jsonFormat オブジェクトのJSON形式を定義するオブジェクト。
export template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void readJsonFileSequentialCore(const std::string& filename, T& out, std::streamsize fileSize,
    std::vector<std::string>& unknownKeysOut, const Provider& provider) {
    std::ifstream ifs(filename, std::ios::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("readJsonFile: Cannot open file " + filename);
    }
    readJsonFileSequentialImpl(
        ifs, filename, out, fileSize, unknownKeysOut, provider);
    ifs.close();
}

/// @brief JSONファイルからオブジェクトを読み込む（逐次処理版）。
/// @tparam T 読み込み対象の型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
/// @param fileSize ファイルサイズ。
/// @param unknownKeysOut 未知キーの収集先。
export template <HasSerializer T>
void readJsonFileSequentialCore(const std::string& filename, T& out, std::streamsize fileSize,
    std::vector<std::string>& unknownKeysOut) {
    readJsonFileSequentialCore(filename, out, fileSize, unknownKeysOut,
        SerializerObjectSerializationProvider{});
}

/// @brief JSONファイルからオブジェクトを読み込む。
/// @tparam T 読み込み対象の型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
/// @param unknownKeysOut 未知キーの収集先。
/// @brief JSONファイルからオブジェクトを読み込む（逐次処理版）。
/// @tparam T 読み込み対象の型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
/// @param unknownKeysOut 未知キーの収集先。
export template <HasSerializer T>
void readJsonFileSequential(const std::string& filename, T& out,
    std::vector<std::string>& unknownKeysOut) {
    readJsonFileSequentialCore(
        filename, out, std::filesystem::file_size(filename), unknownKeysOut);
}

/// @brief JSONファイルからオブジェクトを読み込む（逐次処理版）。
/// @tparam T 読み込み対象の型。
/// @tparam Provider シリアライザー提供者の型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
/// @param unknownKeysOut 未知キーの収集先。
/// @param jsonFormat オブジェクトのJSON形式を定義するオブジェクト。
export template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void readJsonFileSequential(const std::string& filename, T& out,
    std::vector<std::string>& unknownKeysOut, const Provider& provider) {
    readJsonFileSequentialCore(
        filename, out, std::filesystem::file_size(filename), unknownKeysOut,
        provider);
}

/// @brief JSONファイルからオブジェクトを読み込む（逐次処理版、簡易インターフェース）。
/// @tparam T 読み込み対象の型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
export template <HasSerializer T>
void readJsonFileSequential(const std::string& filename, T& out) {
    std::vector<std::string> unknownKeysOut;
    readJsonFileSequential(filename, out, unknownKeysOut);
}

/// @brief JSONファイルからオブジェクトを読み込む（逐次処理版、簡易インターフェース）。
/// @tparam T 読み込み対象の型。
/// @tparam Provider シリアライザー提供者の型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
/// @param jsonFormat オブジェクトのJSON形式を定義するオブジェクト。
export template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void readJsonFileSequential(const std::string& filename, T& out, const Provider& provider) {
    std::vector<std::string> unknownKeysOut;
    readJsonFileSequential(filename, out, unknownKeysOut, provider);
}

/// @brief JSONファイルからオブジェクトを読み込む（並列処理版、内部実装）。
/// @tparam T 読み込み対象の型。
/// @tparam Provider シリアライザー提供者の型。
/// @param ifs 入力元のファイルストリーム（既にオープン済み）。
/// @param filename エラーメッセージ用のファイル名。
/// @param out 読み込み先のオブジェクト。
/// @param unknownKeysOut 未知キーの収集先。
/// @param jsonFormat オブジェクトのJSON形式を定義するオブジェクト。
template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void readJsonFileParallelImpl(std::ifstream& ifs, const std::string& filename, T& out,
    std::vector<std::string>& unknownKeysOut, const Provider& provider) {
    ParallelInputStreamSource inputSource(ifs);
    TokenManager tokenManager;
    StdoutMessageOutput warningOutput;
    JsonTokenizer<ParallelInputStreamSource, TokenManager> tokenizer(
        inputSource, tokenManager, warningOutput);

    std::mutex tokenizerExceptionMutex;
    std::exception_ptr tokenizerException;

    auto& threadPool = rai::common::getGlobalThreadPool();
    std::future<void> tokenizerFuture = threadPool.enqueue([&]() {
        try {
            tokenizer.tokenize();
        } catch (...) {
            auto ex = std::current_exception();
            tokenManager.signalError(ex);
            std::lock_guard<std::mutex> lock(tokenizerExceptionMutex);
            tokenizerException = std::move(ex);
        }
    });

    JsonParser parser(tokenManager);

    try {
        readJsonObject(parser, out, provider);
        unknownKeysOut = std::move(parser.getUnknownKeys());
    } catch (...) {
        tokenizerFuture.wait();
        throw;
    }

    tokenizerFuture.wait();
    {
        std::lock_guard<std::mutex> lock(tokenizerExceptionMutex);
        if (tokenizerException) {
            std::rethrow_exception(tokenizerException);
        }
    }
}

/// @brief JSONファイルからオブジェクトを読み込む（並列処理版）。
/// @tparam T 読み込み対象の型。
/// @tparam Provider シリアライザー提供者の型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
/// @param unknownKeysOut 未知キーの収集先。
/// @note この関数は常に並列処理を行います。小ファイルでも並列化のオーバーヘッドが発生します。
/// @param jsonFormat オブジェクトのJSON形式を定義するオブジェクト。
export template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void readJsonFileParallel(const std::string& filename, T& out,
    std::vector<std::string>& unknownKeysOut, const Provider& provider) {
    std::ifstream ifs(filename, std::ios::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("readJsonFile: Cannot open file " + filename);
    }
    readJsonFileParallelImpl(ifs, filename, out, unknownKeysOut, provider);
}

/// @brief JSONファイルからオブジェクトを読み込む（並列処理版）。
/// @tparam T 読み込み対象の型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
/// @param unknownKeysOut 未知キーの収集先。
/// @note この関数は常に並列処理を行います。小ファイルでも並列化のオーバーヘッドが発生します。
export template <HasSerializer T>
void readJsonFileParallel(const std::string& filename, T& out,
    std::vector<std::string>& unknownKeysOut) {
    readJsonFileParallel(filename, out, unknownKeysOut, SerializerObjectSerializationProvider{});
}

/// @brief JSONファイルからオブジェクトを読み込む（並列処理版、簡易インターフェース）。
/// @tparam T 読み込み対象の型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
export template <HasSerializer T>
void readJsonFileParallel(const std::string& filename, T& out) {
    std::vector<std::string> unknownKeysOut;
    readJsonFileParallel(filename, out, unknownKeysOut);
}

/// @brief JSONファイルからオブジェクトを読み込む（並列処理版、簡易インターフェース）。
/// @tparam T 読み込み対象の型。
/// @tparam Provider シリアライザー提供者の型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
/// @param jsonFormat オブジェクトのJSON形式を定義するオブジェクト。
export template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void readJsonFileParallel(const std::string& filename, T& out, const Provider& provider) {
    std::vector<std::string> unknownKeysOut;
    readJsonFileParallel(filename, out, unknownKeysOut, provider);
}

/// @brief JSONファイルからオブジェクトを読み込む。ファイルサイズに応じて最適な方法を選択。
/// @tparam T 読み込み対象の型。
/// @tparam Provider シリアライザー提供者の型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
/// @param unknownKeysOut 未知キーの収集先。
/// @note 小ファイル（10KB未満）では逐次処理、大ファイルでは並列処理を自動選択します。
/// @param jsonFormat オブジェクトのJSON形式を定義するオブジェクト。
export template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void readJsonFile(const std::string& filename, T& out,
    std::vector<std::string>& unknownKeysOut, const Provider& provider) {
    std::ifstream ifs(filename, std::ios::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("readJsonFile: Cannot open file " + filename);
    }

    // どうしてこの実装にしたか：ファイルサイズに応じて処理方法を切り替える。
    // 小ファイルでは並列化のオーバーヘッド（スレッド起動・同期コスト）が処理時間を上回るため逐次版を使用。
    // すでにファイルを開いているので、std::filesystem::file_sizeよりseekg+tellgの方が速い。
    ifs.seekg(0, std::ios::end);
    std::streamsize fileSize = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    if (fileSize <= smallFileThreshold) {
        // 小ファイルは逐次版を使用
        readJsonFileSequentialImpl(
            ifs, filename, out, fileSize, unknownKeysOut, provider);
    } else {
        // 大ファイルは並列版を使用
        readJsonFileParallelImpl(ifs, filename, out, unknownKeysOut,
            provider);
    }
}

/// @brief JSONファイルからオブジェクトを読み込む。ファイルサイズに応じて最適な方法を選択。
/// @tparam T 読み込み対象の型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
/// @param unknownKeysOut 未知キーの収集先。
/// @note 小ファイル（10KB未満）では逐次処理、大ファイルでは並列処理を自動選択します。
export template <HasSerializer T>
void readJsonFile(const std::string& filename, T& out, std::vector<std::string>& unknownKeysOut) {
    readJsonFile(filename, out, unknownKeysOut, SerializerObjectSerializationProvider{});
}

/// @brief JSONファイルからオブジェクトを読み込む（自動選択版、簡易インターフェース）。
/// @tparam T 読み込み対象の型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
export template <HasSerializer T>
void readJsonFile(const std::string& filename, T& out) {
    std::vector<std::string> unknownKeysOut;
    readJsonFile(filename, out, unknownKeysOut);
}

/// @brief JSONファイルからオブジェクトを読み込む（自動選択版、簡易インターフェース）。
/// @tparam T 読み込み対象の型。
/// @tparam Provider シリアライザー提供者の型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
/// @param jsonFormat オブジェクトのJSON形式を定義するオブジェクト。
export template <typename T, typename Provider>
    requires IsSerializationProvider<Provider, T>
void readJsonFile(const std::string& filename, T& out, const Provider& provider) {
    std::vector<std::string> unknownKeysOut;
    readJsonFile(filename, out, unknownKeysOut, provider);
}

// writeFormat/readFormatメソッドを持つ型専用のオーバーロード

/// @brief writeFormatメソッドを持つ型をJSON形式で文字列化して返す。
/// @tparam T writeFormatメソッドを持つ型。
/// @param obj 変換するオブジェクト。
/// @return JSON形式の文字列。
export template <HasWriteFormat T>
std::string getJsonContent(const T& obj) {
    std::ostringstream oss;
    JsonWriter writer(oss);
    obj.writeFormat(writer);
    return oss.str();
}

/// @brief readFormatメソッドを持つ型をJSON文字列から読み込む。
/// @tparam T readFormatメソッドを持つ型。
/// @param jsonText JSON形式の文字列。
/// @param out 読み込み先のオブジェクト。
export template <HasReadFormat T>
void readJsonString(const std::string& jsonText, T& out) {
    std::string buffer = jsonText;
    buffer.reserve(buffer.size() + aheadSize);

    ReadingAheadBuffer inputSource(std::move(buffer), aheadSize);
    TokenManager tokenManager;
    StdoutMessageOutput warningOutput;
    JsonTokenizer<ReadingAheadBuffer, TokenManager> tokenizer(
        inputSource, tokenManager, warningOutput);
    tokenizer.tokenize();

    JsonParser parser(tokenManager);
    out.readFormat(parser);
}

/// @brief readFormatメソッドを持つ型をJSONファイルから読み込む。
/// @tparam T readFormatメソッドを持つ型。
/// @param filename 入力元のファイル名。
/// @param out 読み込み先のオブジェクト。
export template <HasReadFormat T>
void readJsonFile(const std::string& filename, T& out) {
    std::ifstream ifs(filename, std::ios::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("readJsonFile: Cannot open file " + filename);
    }

    // ファイルサイズを取得
    ifs.seekg(0, std::ios::end);
    std::streamsize fileSize = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    // バッファに読み込み
    std::string buffer;
    buffer.reserve(fileSize + aheadSize);
    buffer.resize(fileSize);
    ifs.read(buffer.data(), buffer.capacity());
    auto bytesRead = ifs.gcount();
    if (ifs.bad()) {
        throw std::runtime_error("readJsonFile: Error reading from file " + filename);
    }
    buffer.resize(bytesRead);

    // トークン化とパース
    ReadingAheadBuffer inputSource(std::move(buffer), aheadSize);
    TokenManager tokenManager;
    StdoutMessageOutput warningOutput;
    JsonTokenizer<ReadingAheadBuffer, TokenManager> tokenizer(
        inputSource, tokenManager, warningOutput);
    tokenizer.tokenize();

    JsonParser parser(tokenManager);
    out.readFormat(parser);
}

}  // namespace rai::serialization
