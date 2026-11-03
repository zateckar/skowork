// Minimal COM helpers: a smart pointer and a generic single-method callback object (WinRT-style typed event
// handlers). IIDs are passed explicitly because MinGW headers declare some of them without definitions.
#pragma once

#include <windows.h>
#include <unknwn.h>

#include <atomic>
#include <functional>
#include <utility>

namespace wb {

template <typename T>
class com_ptr {
public:
    com_ptr() noexcept = default;
    com_ptr(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor)
    explicit com_ptr(T* p, bool add_ref = true) noexcept : p_(p) {
        if (p_ && add_ref) p_->AddRef();
    }
    ~com_ptr() { reset(); }
    com_ptr(const com_ptr& o) noexcept : p_(o.p_) {
        if (p_) p_->AddRef();
    }
    com_ptr(com_ptr&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    com_ptr& operator=(com_ptr o) noexcept {
        std::swap(p_, o.p_);
        return *this;
    }

    T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    T** put() noexcept {
        reset();
        return &p_;
    }
    void** put_void() noexcept { return reinterpret_cast<void**>(put()); }
    void reset() noexcept {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }
    explicit operator bool() const noexcept { return p_ != nullptr; }

    template <typename U>
    HRESULT as(com_ptr<U>& out, REFIID iid) const noexcept {
        return p_ ? p_->QueryInterface(iid, out.put_void()) : E_POINTER;
    }

private:
    T* p_ = nullptr;
};

template <typename Iface, typename Sig>
class com_callback;

template <typename Iface, typename... A>
class com_callback<Iface, HRESULT(A...)> final : public Iface {
public:
    com_callback(const IID& iid, std::function<HRESULT(A...)> fn) : iid_(iid), fn_(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, iid_)) {
            *ppv = static_cast<Iface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(A... a) override { return fn_(a...); }

private:
    ~com_callback() = default;
    IID iid_;
    std::function<HRESULT(A...)> fn_;
    std::atomic<ULONG> refs_{1};
};

// Returns a com_ptr that owns the single initial reference.
template <typename Iface, typename Sig, typename F>
com_ptr<Iface> make_callback(const IID& iid, F&& f) {
    return com_ptr<Iface>(new com_callback<Iface, Sig>(iid, std::function<Sig>(std::forward<F>(f))), false);
}

}  // namespace wb
