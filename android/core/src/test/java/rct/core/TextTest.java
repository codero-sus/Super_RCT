package rct.core;

import org.junit.jupiter.api.Test;

import java.util.Arrays;

import static org.junit.jupiter.api.Assertions.assertEquals;

class TextTest {
    @Test
    void splitsAtLineBreaks() {
        assertEquals(Arrays.asList("aaaa\nbbbb", "cccc"), Text.chunk("aaaa\nbbbb\ncccc", 9));
    }

    @Test
    void cutsLongLinesIntoPieces() {
        assertEquals(Arrays.asList("xxxxxxxxxx", "xxxxxxxxxx", "xxxxx"),
                Text.chunk("x".repeat(25), 10));
    }

    @Test
    void shortTextStaysWhole() {
        assertEquals(Arrays.asList("hello"), Text.chunk("hello", 3500));
    }
}
