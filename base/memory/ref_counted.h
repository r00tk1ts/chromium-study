#ifndef BASE_MEMORY_REF_COUNTED_H_
#define BASE_MEMORY_REF_COUNTED_H_

#include <stddef.h>
#include <stdint.h>

#include <utility>
#include "base/macros.h"
#include "base/memory/scoped_refptr.h"

namespace base {
namespace subtle {

class RefCountedBase {
 public:
  bool HasOneRef() const { return ref_count_ == 1; }
  bool HasAtLeastOneRef() const { return ref_count_ >= 1; }

 protected:
  explicit RefCountedBase(StartRefCountFromZeroTag) {}
  explicit RefCountedBase(StartRefCountFromOneTag) : ref_count_(1) {}

  ~RefCountedBase() {}

  void AddRef() const { AddRefImpl(); }

  bool Release() const {
    ReleaseImpl();
    return ref_count_ == 0;
  }

 private:
  template <typename U>
  friend scoped_refptr<U> base::AdoptRef(U*);

  void AddRefImpl() const { ++ref_count_; }
  void ReleaseImpl() const { --ref_count_; }

  mutable uint32_t ref_count_ = 0;
  static_assert(std::is_unsigned<decltype(ref_count_)>::value,
                "ref_count_ must be an unsigned type.");
  DISALLOW_COPY_AND_ASSIGN(RefCountedBase);
};
}  // namespace subtle

#define REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE()                                   \
  static constexpr ::base::subtle::StartRefCountFromOneTag kRefCountPreference = \
      ::base::subtle::kStartRefCountFromOneTag

template <class T, typename Traits>
class RefCounted;

template <typename T>
struct DefaultRefCountedTraits {
  static void Destruct(const T* x) { RefCounted<T, DefaultRefCountedTraits>::DeleteInternal(x); }
};

template <class T, typename Traits = DefaultRefCountedTraits<T>>
class RefCounted : public subtle::RefCountedBase {
 public:
  static constexpr subtle::StartRefCountFromZeroTag kRefCountPreference =
      subtle::kStartRefCountFromZeroTag;

  RefCounted() : subtle::RefCountedBase(T::kRefCountPreference) {}

  void AddRef() const { subtle::RefCountedBase::AddRef(); }

  void Release() const {
    if (subtle::RefCountedBase::Release()) {
      Traits::Destruct(static_cast<const T*>(this));
    }
  }

 protected:
  ~RefCounted() = default;

 private:
  friend struct DefaultRefCountedTraits<T>;
  template <typename U>
  static void DeleteInternal(const U* x) {
    delete x;
  }

  DISALLOW_COPY_AND_ASSIGN(RefCounted);
};
}  // namespace base
#endif  // BASE_MEMORY_REF_COUNTED_H_
