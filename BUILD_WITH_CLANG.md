# Clang ビルド方法 (Windows向け)

CMake プリセット `CMakePresets.json` を参照。

前提条件
- CMake >= 3.28
- Ninja ビルドシステム (推奨)
- LLVM clang (clang/clang++) または clang-cl（MSVC互換）

推奨プリセット
- `clang-ninja` — clang / clang++ + Ninja
- `clang-cl-ninja` — clang-cl（MSVC ABI）+ Ninja（Visual Studio のヘッダ/ライブラリが必要）

利用例（PowerShell）

1) clang / clang++ を使って設定とビルド

```powershell
# プロジェクトルートで
cmake --preset clang-ninja
cmake --build --preset clang-ninja-debug
```

2) clang-cl を使う（VS の開発者コマンドプロンプト/Developer PowerShell で実行）

```powershell
# Visual Studio Developer PowerShell から実行
cmake --preset clang-cl-ninja
cmake --build --preset clang-cl-ninja-debug
```

3) テストを実行する

```powershell
# ビルド後にテスト実行（このリポジトリは out-of-source ビルドを推奨しています）
cd build/clang-ninja
ctest --output-on-failure -V
```

- `JsonTest` — 単体機能テスト（7 個のテストケースなど）
- `JsonBenchmark` — ベンチマークをテストとしてラップしたもの（実行時間が長くなることがあります）

ベンチマークや個別テストを実行したい時:

```powershell
# JsonBenchmark のみ実行
cd build/clang-ninja
ctest -R JsonBenchmark -V

# JsonTest のみ実行
ctest -R JsonTest -V
```

注意（ベンチマーク実行）:
- `JsonBenchmark` の各ケースは多数の反復やファイル I/O を行うため実行に時間がかかります。CI 等で不要ならフィルタを使って実行を制限してください（例: `ctest -R JsonTest`）。
- `JsonBenchmark.cpp` の一部 import 名はモジュール名 `rai.compiler.io.*` を使用するよう修正済みです（ビルドのためにモジュール名とファイルのエクスポート名が一致する必要があります）。

注意
- `clang-cl` を選ぶ際は Visual Studio のヘッダ/ライブラリが必要です（通常は Visual Studio の Developer Command Prompt / Developer PowerShell を使うと環境が整います）。
- システムに clang がインストールされ PATH にあることを確認してください。

問題があれば、この `CMakePresets.json` を調整して使いたい clang のパス（例: `CMAKE_C_COMPILER`, `CMAKE_CXX_COMPILER`）や追加のキャッシュ変数を設定してください。
