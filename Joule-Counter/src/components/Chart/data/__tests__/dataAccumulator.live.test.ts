/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-4-Clause
 */

/*
 * Drives the accumulator the way live mode does: the window slides over a
 * growing record and the cached result is merged with fresh edge data every
 * frame. Every frame must come back with the same number of points; a frame
 * that loses the trace is what the user sees as a flicker.
 */

import {
    frameSize,
    indexToTimestamp,
    timestampToIndex,
} from '../../../../globals';
import dataAccumulatorInitialiser from '../dataAccumulator';

const SAMPLES_PER_SECOND = 50_000;
const TOTAL_SECONDS = 25;
const record = Buffer.alloc(TOTAL_SECONDS * SAMPLES_PER_SECOND * frameSize);
// Samples appended so far; grows during the run like a live session.
let latestIndex = 0;

jest.mock('../../../../features/recovery/SessionsListFileHandler', () => ({
    ReadSessions: jest.fn(() => []),
    WriteSessions: jest.fn(),
    AddSession: jest.fn(),
    RemoveSessionByFilePath: jest.fn(),
    ClearSessions: jest.fn(),
    SessionFlag: { NotRecovered: 0, Recovered: 1, PPK2Loaded: 2 },
}));

jest.mock('../../../../globals', () => {
    const actual = jest.requireActual('../../../../globals');
    return {
        ...actual,
        DataManager: () => ({
            getSamplingTime: () => 1e6 / SAMPLES_PER_SECOND,
            getSamplesPerSecond: () => SAMPLES_PER_SECOND,
            getTimestamp: () =>
                actual.indexToTimestamp(latestIndex - 1, SAMPLES_PER_SECOND),
            getTotalSavedRecords: () => latestIndex,
            getNumberOfSamplesInWindow: (windowDuration: number) =>
                actual.timestampToIndex(windowDuration, SAMPLES_PER_SECOND),
            getData: (buffer: Buffer, fromTime: number, toTime: number) => {
                const first = actual.timestampToIndex(
                    fromTime,
                    SAMPLES_PER_SECOND,
                );
                const last = Math.min(
                    actual.timestampToIndex(toTime, SAMPLES_PER_SECOND),
                    latestIndex - 1,
                );
                const bytes = Math.max(
                    0,
                    (last - first + 1) * actual.frameSize,
                );
                record.copy(
                    buffer,
                    0,
                    first * actual.frameSize,
                    first * actual.frameSize + bytes,
                );
                return Promise.resolve(new actual.FileData(buffer, bytes));
            },
        }),
    };
});

const writeSample = (index: number, current: number, voltage: number) => {
    const off = index * frameSize;
    record.writeFloatLE(current, off);
    record.writeFloatLE(voltage, off + 4);
    record.writeUInt16BE(0xaaaa, off + 8);
};

describe('live-mode accumulation', () => {
    beforeAll(() => {
        // 9 uA baseline with a 1 mA pulse every 71.7 ms, at 2.33 V.
        for (let i = 0; i < TOTAL_SECONDS * SAMPLES_PER_SECOND; i += 1) {
            const inPulse = i % 3585 < 22;
            writeSample(i, inPulse ? 1000 : 9, 2.33);
        }
    });

    it('never loses the trace between frames', async () => {
        const acc = dataAccumulatorInitialiser();
        const windowDuration = 10e6; // µs
        const frameStep = Math.round(SAMPLES_PER_SECOND / 30); // ~33 ms of new data per frame
        const anomalies: string[] = [];
        let prevPoints: number | undefined;

        // Deterministic jitter: USB delivers 512-sample blocks, so frames see
        // anywhere from nothing new to a few blocks.
        let seed = 12345;
        const rand = () => {
            seed = (seed * 1103515245 + 12345) % 2 ** 31;
            return seed / 2 ** 31;
        };

        latestIndex = 12 * SAMPLES_PER_SECOND; // start with 12 s recorded
        while (
            latestIndex + 3 * frameStep <
            TOTAL_SECONDS * SAMPLES_PER_SECOND
        ) {
            latestIndex += 512 * Math.floor(rand() * 7); // 0..6 blocks
            const end = indexToTimestamp(latestIndex - 1, SAMPLES_PER_SECOND);
            const begin = Math.max(0, end - windowDuration);

            // eslint-disable-next-line no-await-in-loop
            const result = await acc.process(
                begin,
                end,
                [],
                1000,
                windowDuration,
            );

            const points = result.ampereLineData.filter(
                p => p && p.x !== undefined && p.y != null,
            ).length;
            const powerPoints = result.powerLine.filter(
                p => p && p.x !== undefined && p.count > 0,
            ).length;
            const xs = result.ampereLineData
                .filter(p => p && p.x !== undefined)
                .map(p => p.x as number);
            const span = xs.length ? Math.max(...xs) - Math.min(...xs) : 0;

            if (
                prevPoints !== undefined &&
                (points < prevPoints * 0.9 || powerPoints < points / 2 - 5)
            ) {
                anomalies.push(
                    `at ${(end / 1e6).toFixed(2)} s: ${points} current points (was ${prevPoints}), ${powerPoints} power points, span ${(span / 1e6).toFixed(2)} s, ${timestampToIndex(span, SAMPLES_PER_SECOND)} samples`,
                );
            }
            prevPoints = points;
        }

        expect(anomalies).toEqual([]);
    }, 120_000);
});
