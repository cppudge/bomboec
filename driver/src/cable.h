/*++

Module Name:

    cable.h

Abstract:

    Общий кольцевой буфер между render-endpoint (Speakers) и capture-endpoint
    (Microphone) виртуального кабеля. Render-поток кладёт байты из своего
    WaveRT-буфера (ReadBytes), capture-поток забирает их в свой (WriteBytes).
    Оба endpoint'а работают в одном формате (48 kHz, 2 ch, 16-bit PCM),
    поэтому буфер байтовый, без конвертации.

    WaveRT сдвигает позиции на произвольное число байт, поэтому граница
    кадра (4 байта) отслеживается по фазе позиции потока: голова буфера
    хранит фазу render-потока, а при старте чтения хвост подгоняется под фазу
    capture-потока. Иначе capture читает сэмплы со сдвигом на байт.

    Чтение начинается только после накопления primeBytes (задержка кабеля);
    всё, что накопилось сверх primeBytes, пока capture не читал, при старте
    чтения отбрасывается: обе стороны ведут позиции от одного QPC, поэтому
    backlog сам по себе никогда не рассосался бы. При опустошении capture
    получает тишину и снова ждёт накопления. Переполнение отбрасывает самые
    старые данные и требует повторного выравнивания.

--*/

#ifndef _BOMBOEC_CABLE_H_
#define _BOMBOEC_CABLE_H_

#define CABLE_BLOCK_ALIGN 4

class CCableBuffer
{
public:
    NTSTATUS Init(_In_ ULONG capacityBytes, _In_ ULONG primeBytes);
    void Free();

    // Сброс при старте render-потока с нулевой позиции.
    void Reset();

    // Producer (render stream). streamPos: линейная позиция первого байта data
    // в render-потоке (для фазы кадра). IRQL <= DISPATCH_LEVEL.
    void Write(_In_reads_bytes_(bytes) const BYTE* data, _In_ ULONG bytes, _In_ ULONGLONG streamPos);

    // Consumer (capture stream). Всегда заполняет все bytes (данные или тишина).
    // streamPos: линейная позиция первого байта data в capture-потоке.
    void Read(_Out_writes_bytes_all_(bytes) BYTE* data, _In_ ULONG bytes, _In_ ULONGLONG streamPos);

    ULONG Available() const { return m_count; }

private:
    KSPIN_LOCK  m_lock;
    BYTE*       m_buffer;
    ULONG       m_capacity;
    ULONG       m_prime;
    ULONG       m_head;         // индекс записи
    ULONG       m_count;        // доступно байт
    ULONG       m_headPhase;    // фаза кадра render-потока в m_head (0..3)
    BOOLEAN     m_primed;
    ULONGLONG   m_underruns;
    ULONGLONG   m_overruns;
    ULONGLONG   m_realigns;
    ULONGLONG   m_trimmedBytes; // отброшено при прайме сверх m_prime
};

extern CCableBuffer g_Cable;

#endif // _BOMBOEC_CABLE_H_
