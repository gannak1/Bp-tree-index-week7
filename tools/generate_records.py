import argparse
import random
import string


def random_name(rng: random.Random) -> str:
    length = rng.randint(5, 12)
    return "".join(rng.choice(string.ascii_letters.lower()) for _ in range(length))


def generate_sql(count: int, output_path: str, seed: int) -> None:
    rng = random.Random(seed)

    with open(output_path, "w", encoding="utf-8", newline="\n") as file:
        for _ in range(count):
            name = random_name(rng)
            age = rng.randint(1, 100)
            file.write(f"INSERT INTO users VALUES ('{name}', {age});\n")


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate INSERT SQL records for the users table.")
    parser.add_argument("--count", type=int, default=1_000_000, help="number of INSERT statements to generate")
    parser.add_argument("--output", default="generated_records.sql", help="output SQL file path")
    parser.add_argument("--seed", type=int, default=20260415, help="random seed for reproducible data")
    args = parser.parse_args()

    generate_sql(args.count, args.output, args.seed)
    print(f"Generated {args.count} INSERT statements at '{args.output}' with seed {args.seed}.")


if __name__ == "__main__":
    main()
