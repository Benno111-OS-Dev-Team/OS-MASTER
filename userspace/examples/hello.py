# Hello World in Python for OS8
# Run with: run hello.py

def greet(name):
    return f"Hello, {name}!"

def main():
    print("=" * 40)
    print("  Welcome to OS8 Python Demo")
    print("=" * 40)
    
    message = greet("OS8 User")
    print(message)
    
    # Simple calculation demo
    numbers = [1, 2, 3, 4, 5]
    total = sum(numbers)
    print(f"Sum of {numbers} = {total}")
    
    print("\nPython is working on OS8!")

if __name__ == "__main__":
    main()
