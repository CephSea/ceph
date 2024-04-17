// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2020 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */

#include "common/ceph_timer.h"

#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <vector>

using namespace std::literals;

namespace {
template<typename TC> void run_some()
{
    static constexpr auto MAX_FUTURES = 5;
    ceph::timer<TC> timer;
    std::vector<std::future<void>> futures;
    for (auto i = 0; i < MAX_FUTURES; ++i) {
        auto t = TC::now() + 2s;
        std::promise<void> p;
        futures.push_back(p.get_future());
        timer.add_event(t, [p = std::move(p)]() mutable { p.set_value(); });
    }
    for (auto& f : futures) f.get();
}

template<typename TC> void run_orderly()
{
    ceph::timer<TC> timer;

    std::future<typename TC::time_point> first;
    std::future<typename TC::time_point> second;


    {
        std::promise<typename TC::time_point> p;
        second = p.get_future();
        timer.add_event(4s, [p = std::move(p)]() mutable { p.set_value(TC::now()); });
    }
    {
        std::promise<typename TC::time_point> p;
        first = p.get_future();
        timer.add_event(2s, [p = std::move(p)]() mutable { p.set_value(TC::now()); });
    }

    EXPECT_LT(first.get(), second.get());
}

struct Destructo
{
    bool armed = true;
    std::promise<void> p;

    Destructo(std::promise<void>&& p)
        : p(std::move(p))
    {}
    Destructo(const Destructo&) = delete;
    Destructo& operator=(const Destructo&) = delete;
    Destructo(Destructo&& rhs)
    {
        p = std::move(rhs.p);
        armed = rhs.armed;
        rhs.armed = false;
    }
    Destructo& operator=(Destructo& rhs)
    {
        p = std::move(rhs.p);
        rhs.armed = false;
        armed = rhs.armed;
        rhs.armed = false;
        return *this;
    }

    ~Destructo()
    {
        if (armed) p.set_value();
    }
    void operator()() const { FAIL(); }
};

template<typename TC> void cancel_all()
{
    ceph::timer<TC> timer;
    static constexpr auto MAX_FUTURES = 5;
    std::vector<std::future<void>> futures;
    for (auto i = 0; i < MAX_FUTURES; ++i) {
        std::promise<void> p;
        futures.push_back(p.get_future());
        timer.add_event(100s + i * 1s, Destructo(std::move(p)));
    }
    timer.cancel_all_events();
    for (auto& f : futures) f.get();
}

template<typename TC> void cancellation()
{
    ceph::timer<TC> timer;
    {
        std::promise<void> p;
        auto f = p.get_future();
        auto e = timer.add_event(100s, Destructo(std::move(p)));
        EXPECT_TRUE(timer.cancel_event(e));
    }
    {
        std::promise<void> p;
        auto f = p.get_future();
        auto e = timer.add_event(1s, [p = std::move(p)]() mutable { p.set_value(); });
        f.get();
        EXPECT_FALSE(timer.cancel_event(e));
    }
}
}   // namespace

TEST(RunSome, Steady)
{
    run_some<std::chrono::steady_clock>();
}
TEST(RunSome, Wall)
{
    run_some<std::chrono::system_clock>();
}

TEST(RunOrderly, Steady)
{
    run_orderly<std::chrono::steady_clock>();
}
TEST(RunOrderly, Wall)
{
    run_orderly<std::chrono::system_clock>();
}

TEST(CancelAll, Steady)
{
    cancel_all<std::chrono::steady_clock>();
}
TEST(CancelAll, Wall)
{
    cancel_all<std::chrono::system_clock>();
}
