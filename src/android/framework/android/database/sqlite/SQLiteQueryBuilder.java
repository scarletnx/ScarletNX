// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.database.sqlite;

/** Builds SELECT statements. */
public class SQLiteQueryBuilder {
    public static String buildQueryString(boolean distinct, String tables, String[] columns, String where, String groupBy,
                                          String having, String orderBy, String limit) {
        if ((groupBy == null || groupBy.isEmpty()) && having != null && !having.isEmpty())
            throw new IllegalArgumentException("HAVING clauses are only permitted when using a groupBy clause");
        StringBuilder q = new StringBuilder(120).append("SELECT ");
        if (distinct) q.append("DISTINCT ");
        if (columns != null && columns.length != 0) {
            for (int i = 0; i < columns.length; ++i) q.append(i > 0 ? ", " : "").append(columns[i]);
            q.append(' ');
        } else {
            q.append("* ");
        }
        q.append("FROM ").append(tables);
        if (where != null && !where.isEmpty()) q.append(" WHERE ").append(where);
        if (groupBy != null && !groupBy.isEmpty()) q.append(" GROUP BY ").append(groupBy);
        if (having != null && !having.isEmpty()) q.append(" HAVING ").append(having);
        if (orderBy != null && !orderBy.isEmpty()) q.append(" ORDER BY ").append(orderBy);
        if (limit != null && !limit.isEmpty()) q.append(" LIMIT ").append(limit);
        return q.toString();
    }
}
