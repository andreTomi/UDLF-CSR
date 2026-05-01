#ifndef SINGLETON_HPP
#define SINGLETON_HPP

template <typename T>
class Singleton {
 public:
  static T &getInstance() {
    static T instance;
    return instance;
  }

  // Delete copy and move constructors
  Singleton(const Singleton &) = delete;
  Singleton(Singleton &&) = delete;
  Singleton &operator=(const Singleton &) = delete;
  Singleton &operator=(Singleton &&) = delete;

 protected:
  Singleton() = default;
  virtual ~Singleton() = default;
};

#endif  // SINGLETON_HPP
