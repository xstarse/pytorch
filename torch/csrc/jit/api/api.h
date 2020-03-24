#pragma once
#include <c10/util/Exception.h>
#include <torch/csrc/autograd/variable.h>
#include <torch/csrc/jit/runtime/argument_spec.h>
#include <torch/csrc/jit/runtime/graph_executor.h>
#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/ir/named_value.h>
#include <torch/csrc/jit/passes/shape_analysis.h>
#include <torch/csrc/jit/api/object.h>
#include <torch/csrc/jit/frontend/source_range.h>

#include <torch/csrc/WindowsTorchApiMacro.h>
#include <torch/csrc/api/include/torch/ordered_dict.h>
#include <torch/csrc/jit/api/compilation_unit.h>
#include <torch/csrc/utils/memory.h>

#include <ATen/core/function_schema.h>
#include <ATen/core/qualified_name.h>
#include <c10/util/ArrayRef.h>
#include <c10/util/Optional.h>

#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace torch {
  // TODO: Find the exsiting typedefs for these
  using Device = at::Device;
  using Dtype = at::ScalarType;
  using Tensor = at::Tensor;
}

namespace torch {
namespace jit {
namespace api {

template <typename T>
struct iterator {
  typedef T value_type;
  typedef T& reference;
  typedef T* pointer;
  typedef std::forward_iterator_tag iterator_category;

  iterator() = default;
  iterator(pointer the_pointer) : pointer_(std::move(the_pointer)) {}

  reference operator*() const {
    return *pointer_;
  }
  pointer operator->() const {
    return *pointer_;
  }

  iterator operator++() {
    ++pointer_;
  }

  bool operator==(const iterator& rhs) {
    return rhs.pointer_ == pointer_;
  }
  bool operator!=(const iterator& rhs) {
    return !(this == rhs);
  }

private:
  pointer pointer_;
};

template <typename T>
struct iterable {
  iterable(iterator<T> begin, iterator<T> end)
      : end_(std::move(end)), begin_(std::move(begin)) {}
  iterator<T> begin() const;
  iterator<T> end() const;

private:
  iterator<T> begin_;
  iterator<T> end_;
};

template <typename T>
struct Field {
  const std::string name;
  T value;
};

struct Dict;
struct List;
struct Tuple;
struct Object;
struct Value;
struct ClassType;
struct ListType;
struct DictType;
struct TupleType;
struct ObjectType;
struct Module;

struct Type {
  /// Typing relationship checks
  bool isSubtypeOf(const Type& rhs);

  /// Type casting
  template<typename T>
  T expect() {

  }

  std::string python_str() const noexcept;

  static const Type Bool();
  static const Type DeviceObject();
  static const Type Float();
  static const Type Int();
  static const Type Layout();
  static const Type None();
  static const Type String();
  static const Type Tensor();

protected:
  Type(c10::TypePtr type) : type_(std::move(type)) {}
  c10::TypePtr type_;
  friend struct ClassType;
};

struct class_attribute_type_iterator : iterator<Field<Type>> {
  class_attribute_type_iterator(c10::ClassTypePtr type, size_t index)
      : type_(type), index_(std::move(index)) {
    set_current_field();
  }

  reference operator*() const {
    return current_field_;
  }
  pointer operator->() const {
    return &current_field_;
  }

  class_attribute_type_iterator operator++() {
    ++index_;
    set_current_field();
  }

  bool operator==(const class_attribute_type_iterator& rhs) {
    return rhs.type_->isSubtypeOf(type_) && type_->isSubtypeOf(rhs.type_) &&
        rhs.index_ == index_;
  }
  bool operator!=(const class_attribute_type_iterator& rhs) {
    return !(*this == rhs);
  }

private:
  void set_current_field() {
    current_field_ = Field<Type>{type_->attributeNames().at(index_),
                                 type_->containedTypes().at(index_)};
  }
  Field<Type> current_field_;
  c10::ClassTypePtr type_;
  size_t index_;
};

/// TODO: How to query for class types by qualified name?
struct ClassType : public Type {
  Type attr(const std::string& name);
  bool hasattr(const std::string& name);
  iterable<Field<Type>> attributes();

private:
  ClassType(c10::ClassTypePtr type) : Type(std::move(type)) {}
};

struct ListType : public Type {
  ListType(Type element);
  Type element() const;
};

struct DictType : public Type {
  DictType(Type key, Type value);
  Type key() const;
  Type value() const;
};

struct TupleType : public Type {
  TupleType(std::vector<Type> values);
  TupleType(std::vector<std::string> field_names, std::vector<Type> values);
  const std::vector<Type>& elements() const;
};

struct OptionalType : public Type {
  OptionalType(Type element);
  Type element() const;
};

// All the methods that go on a Value get defined here. This makes it easy for
// accessor objects, used to represent the LHS of assignments to also have these
// methods through inheritance. This design is adapted from pybind11
struct Value {
  template <typename T>
  Value(T&& value) : ivalue_(std::forward<T>(value)) {}
  Value(Value& value);
  Value(Value&& value);
  Value(const Value& value);

  Type type() const;

  std::ostream& operator<<(std::ostream& o);

  /// Escape hatch to internal type
  const IValue& ivalue() const {
    return ivalue_;
  }

  bool isinstance(const Type& t);

  template <typename T>
  T to() const;

  /// Check if a call to `.to<T>()` will succeed
  template <typename T>
  T isinstance() const;

  bool isNone() const;
  Value toNone() const;

  bool isTensor() const;
  torch::Tensor toTensor() const;

  bool isDouble() const;
  double toDouble() const;

  bool isInt() const;
  int64_t toInt() const;

  bool isBool() const;
  void toBool() const;

  bool isTuple() const;
  Tuple toTuple() const;

  bool isString() const;
  std::string toString() const;

  bool isList() const;
  List toList() const;

  bool isDict() const;
  Dict toDict() const;

  bool isDevice() const;
  torch::Device toDevice() const;

  bool isObject() const;
  Object toObject() const;

 private:
  IValue ivalue_;
};

template <typename Policy>
struct Accessor : public Value {
  using key_type = typename Policy::key_type;

  Accessor(jit::IValue obj, key_type key);
  Accessor(const Accessor&) = default;
  Accessor(Accessor&&) = default;

  template <typename T>
  void operator=(T&& value) &&;
  void operator=(const Accessor& a) &&;

 private:
  const jit::IValue& ivalue() const;
  jit::IValue obj_;
  key_type key_;
};

namespace detail {
  struct ListPolicy {
    using key_type = size_t;
    static jit::IValue get(const jit::IValue& obj, const key_type& key);
    static void set(
        const jit::IValue& obj,
        const key_type& key,
        const jit::IValue value);
  };

  struct DictAccessorPolicy {
    using key_type = jit::IValue;
    static jit::IValue get(const jit::IValue& obj, const key_type& key) {
      return obj.toGenericDict().at(key);
    }
    static void set(
        const jit::IValue& obj,
        const key_type& key,
        const jit::IValue value);
  };

  struct AttrPolicy {
    using key_type = std::string;
    static jit::IValue get(const jit::IValue& obj, const key_type& key);
    static void set(
        const jit::IValue& obj,
        const key_type& key,
        const jit::IValue value);
  };
}

using ListAccessor = Accessor<detail::ListPolicy>;
using DictAccessor = Accessor<detail::DictAccessorPolicy>;

struct List : public Value {
  List(TypePtr type, std::initializer_list<Value> = {});
  // list must not be empty
  List(std::initializer_list<Value>);
  ListAccessor operator[](size_t i) const;
  size_t size();
  iterator<Value> begin();
  iterator<Value> end();
  void append(Value v);
};

struct Tuple : public Value {
  Value operator[](size_t i) const;
  // named tuple access
  Value operator[](const std::string& name) const;
  size_t size() const;
  iterator<Value> begin();
  iterator<Value> end();
};

struct DictEntry {
  Value key;
  Value value;
};

struct Dict : public Value {
  Dict(TypePtr key, TypePtr value, std::initializer_list<DictEntry> = {});
  // list must not be empty
  Dict(std::initializer_list<DictEntry>);
  DictAccessor operator[](Value key) const;
  size_t size() const;
  iterator<DictEntry> begin();
  iterator<DictEntry> end();
};

using AttrAccessor = Accessor<detail::AttrPolicy>;

struct Object : public Value {
  bool hasattr(const std::string& name);
  AttrAccessor attr(const std::string& name);
  Value attr(const std::string& name, const Value& or_else);

  template <typename... Args>
  Value call(const std::string& method_name, Args... args);
  Value call(const std::string& method_name,
      std::vector<Value> args,
      std::vector<Field<Value>> kwargs);
  iterable<Field<Value>> begin() const;
  iterable<Field<Value>> end() const;
};

struct Module : public Object {
  // Module is an object, but also exposes the nn.Module API:
  template <typename... Args>
  Value forward(Args&&... args);

  const std::string& name() const noexcept;

  // Fetch Modules
  iterable<Module> modules(bool include_self = true);
  iterable<Field<Module>> named_modules(bool include_self = true);
  iterable<Module> children();
  iterable<Field<Module>> named_children();

  // Fetch Attributes
  iterable<torch::Tensor> parameters() const;
  iterable<Field<torch::Tensor>> named_parameters(bool recurse = true) const;
  iterable<torch::Tensor> buffers() const;
  iterable<Field<torch::Tensor>> named_buffers(bool recurse = true) const;

  void apply(std::function<void(Module)>);

  void to(torch::Device device, torch::Dtype dtype, bool non_blocking = false);
  void to(torch::Dtype dtype, bool non_blocking = false);
  void to(torch::Device device, bool non_blocking = false);

  void train(bool on = true);
  void eval();
  bool is_training() const noexcept;
  void requires_grad_() const;

  // shortcuts to `.attr()` for pairity with nn::Module API
  torch::Tensor& register_parameter(
      std::string name,
      torch::Tensor tensor,
      bool requires_grad = true);
  torch::Tensor& register_buffer(
      std::string name,
      torch::Tensor tensor);
  Module& register_module(
      std::string name,
      Module module);
  Module& replace_module(
      std::string name,
      Module module);

  void zero_grad();

  // convience method to lookup classes that were used in this Module by name
  ClassType Class(const std::string& qualified_name);
};

Module load(const std::string& path);


} // namespace api
} // namespace jit
} // namespace torch
