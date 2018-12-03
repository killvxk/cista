#pragma once

#include <iostream>
#include <limits>
#include <map>
#include <vector>

#include "cista/containers/string.h"
#include "cista/containers/unique_ptr.h"
#include "cista/containers/vector.h"
#include "cista/offset_t.h"
#include "cista/reflection/for_each_field.h"
#include "cista/targets/buf.h"
#include "cista/targets/file.h"

namespace cista {

// =============================================================================
// SERIALIZE
// -----------------------------------------------------------------------------
struct pending_offset {
  void* origin_ptr_;
  offset_t pos_;
};

template <typename Target>
struct serialization_context {
  explicit serialization_context(Target& t) : t_{t} {}

  offset_t write(void const* ptr, offset_t const size, offset_t alignment = 0) {
    return t_.write(ptr, size, alignment);
  }

  template <typename T>
  void write(offset_t const pos, T const& val) {
    t_.write(pos, val);
  }

  std::map<void*, offset_t> offsets_;
  std::vector<pending_offset> pending_;
  Target& t_;
};

template <typename Ctx, typename T>
void serialize(Ctx& c, T const* origin, offset_t const pos) {
  using Type = std::remove_reference_t<std::remove_const_t<T>>;
  if constexpr (!std::is_scalar_v<Type>) {
    cista::for_each_ptr_field(*origin, [&](auto& member) {
      auto const member_offset =
          static_cast<offset_t>(reinterpret_cast<char const*>(member) -
                                reinterpret_cast<char const*>(origin));
      serialize(c, member, pos + member_offset);
    });
  } else if constexpr (std::is_pointer_v<Type>) {
    if (*origin == nullptr) {
      c.write(pos, std::numeric_limits<offset_t>::max());
      return;
    }
    if (auto const it = c.offsets_.find(*origin); it != end(c.offsets_)) {
      c.write(pos, it->second);
    } else {
      c.pending_.emplace_back(pending_offset{*origin, pos});
    }
  }
}

template <typename Ctx, typename T>
void serialize(Ctx& c, cista::vector<T> const* origin, offset_t const pos) {
  auto const size = sizeof(T) * origin->used_size_;
  auto const start = origin->el_ != nullptr
                         ? c.write(origin->el_, size, std::alignment_of_v<T>)
                         : std::numeric_limits<offset_t>::max();

  c.write(pos + offsetof(cista::vector<T>, el_), start);
  c.write(pos + offsetof(cista::vector<T>, allocated_size_),
          origin->used_size_);
  c.write(pos + offsetof(cista::vector<T>, self_allocated_), false);

  if (origin->el_ != nullptr) {
    auto i = 0u;
    for (auto it = start; it != start + size; it += sizeof(T)) {
      serialize(c, origin->el_ + i++, it);
    }
  }
}

template <typename Ctx>
void serialize(Ctx& c, cista::string const* origin, offset_t const pos) {
  if (origin->is_short()) {
    return;
  }

  auto const start =
      c.write(origin->data(), origin->size(), std::alignment_of_v<char>);
  c.write(pos + offsetof(cista::string, h_.ptr_), start);
  c.write(pos + offsetof(cista::string, h_.self_allocated_), false);
}

template <typename Ctx, typename T>
void serialize(Ctx& c, cista::unique_ptr<T> const* origin, offset_t const pos) {
  auto const start =
      origin->el_ != nullptr
          ? c.write(origin->el_, sizeof(T), std::alignment_of_v<T>)
          : std::numeric_limits<offset_t>::max();

  c.write(pos + offsetof(cista::unique_ptr<T>, el_), start);
  c.write(pos + offsetof(cista::unique_ptr<T>, self_allocated_), false);

  if (origin->el_ != nullptr) {
    c.offsets_[origin->el_] = start;
    serialize(c, origin->el_, start);
  }
}

template <typename Target, typename T>
void serialize(Target& t, T& value) {
  serialization_context<Target> c{t};

  serialize(
      c, &value,
      c.write(
          &value, sizeof(value),
          std::alignment_of_v<
              std::remove_reference_t<std::remove_const_t<decltype(value)>>>));

  for (auto& p : c.pending_) {
    if (auto const it = c.offsets_.find(p.origin_ptr_); it != end(c.offsets_)) {
      c.write(p.pos_, it->second);
    } else {
      std::cout << "warning: dangling pointer " << p.origin_ptr_
                << " serialized at offset " << p.pos_ << "\n";
    }
  }
}

template <typename T>
byte_buf serialize(T& el) {
  auto b = buf{};
  serialize(b, el);
  return std::move(b.buf_);
}

// =============================================================================
// DESERIALIZE
// -----------------------------------------------------------------------------
struct deserialization_context {
  deserialization_context(bool checked, uint8_t* from, uint8_t* to)
      : checked_{checked}, from_{from}, to_{to} {}

  template <typename T, typename Ptr>
  T deserialize(Ptr* ptr) const {
    auto const offset = reinterpret_cast<offset_t>(ptr);
    if (offset == std::numeric_limits<offset_t>::max()) {
      return nullptr;
    }
    return reinterpret_cast<T>(from_ + offset);
  }

  template <typename T>
  void check(T* el, size_t size) const {
    auto const* pos = reinterpret_cast<uint8_t*>(el);
    if ((checked_ && to_) && (pos < from_ || pos + size > to_)) {
      throw std::runtime_error("pointer out of bounds");
    }
  }

  bool checked_;
  uint8_t *from_, *to_;
};

template <typename T>
void deserialize(deserialization_context const& c, T* el) {
  using written_type_t = std::remove_reference_t<std::remove_const_t<T>>;
  if constexpr (std::is_pointer_v<written_type_t>) {
    *el = c.deserialize<written_type_t>(*el);
    c.check(*el, sizeof(*std::declval<written_type_t>()));
  } else if constexpr (std::is_scalar_v<written_type_t>) {
    return;
  } else {
    cista::for_each_ptr_field(*el, [&](auto& f) { deserialize(c, f); });
  }
}

template <typename T>
void deserialize(deserialization_context const& c, cista::vector<T>* el) {
  el->el_ = c.deserialize<T*>(el->el_);
  for (auto& m : *el) {
    deserialize(c, &m);
  }
}

inline void deserialize(deserialization_context const& c, cista::string* el) {
  if (el->is_short()) {
    return;
  } else {
    el->h_.ptr_ = c.deserialize<char*>(el->h_.ptr_);
  }
}

template <typename T>
void deserialize(deserialization_context const& c, cista::unique_ptr<T>* el) {
  el->el_ = c.deserialize<T*>(el->el_);
  deserialize(c, el->el_);
}

template <typename T>
T* deserialize(uint8_t* from, uint8_t* to = nullptr, bool checked = true) {
  deserialization_context c{checked, from, to};
  auto const el = reinterpret_cast<T*>(from);
  deserialize(c, el);
  return el;
}

template <typename T, typename Container>
T* deserialize(Container& c, bool checked = true) {
  return deserialize<T>(&c[0], &c[c.size()], checked);
}

}  // namespace cista