"""Count handwritten application source, excluding tests and generated files."""
from pathlib import Path


def main():
    root = Path(__file__).resolve().parent.parent
    paths = sorted((root / "src").glob("*.cpp"))
    paths += sorted((root / "include").rglob("*.h"))
    lines = [
        "范围：手写 src/*.cpp 与 include/**/*.h；不含测试、工具、第三方依赖和构建目录。",
        "口径：物理行数与非空行数；非空行仍含注释，不冒称纯代码 SLOC。",
        "文件\t物理行数\t非空行数",
    ]
    total = nonempty = 0
    for path in paths:
        source = path.read_text().splitlines()
        count = sum(bool(line.strip()) for line in source)
        total += len(source)
        nonempty += count
        lines.append(f"{path.relative_to(root)}\t{len(source)}\t{count}")
    lines.append(f"合计\t{total}\t{nonempty}")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
