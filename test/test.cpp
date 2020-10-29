#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include <iostream>

using namespace std;
using namespace base;

class MyClass : public RefCounted<MyClass> {
 public:
  void test() { static int m = 0; cout << "test" << m++ << endl;}
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

 private:
  friend class base::RefCounted<MyClass>;
  ~MyClass() { cout << "destroy MyClass" << endl;}
};

int main(){
  MyClass *ptr = new MyClass();
  scoped_refptr<MyClass> smart_ptr2 = WrapRefCounted(ptr);
  smart_ptr2->test();
  ptr->test();

  return 0;
}
