/* Copyright (C) 2025 Interactive Brokers LLC. All rights reserved. This code is subject to the terms
 * and conditions of the IB API Non-Commercial License or the IB API Commercial License, as applicable. */

package com.ib.client;

public enum OptionExerciseType {
    None(-1),
    Exercise(1),
    Lapse(2),
    DoNothing(3),
    Assigned(100),
    AutoexerciseClearing(101),
    Expired(102),
    Netting(103),
    AutoexerciseTrading(200);

    private int m_val;

    OptionExerciseType(int v) {
         m_val = v;
    }

    public int val() {
        return m_val;
    }

    public static OptionExerciseType fromInt(int n) {
        for (OptionExerciseType i : OptionExerciseType.values())
            if (i.val() == n)
                return i;

        throw new IllegalArgumentException("Error: " + n + " is not a valid value for enum OptionExerciseType");
    }
}