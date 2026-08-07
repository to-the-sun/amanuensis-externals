import unittest
from visualizer import should_pulse_red

class TestVisualizerPaletteRedPulse(unittest.TestCase):

    def test_direct_string_dictionary_entry(self):
        b_dict = {"palette": "dictionary_entry"}
        self.assertTrue(should_pulse_red(b_dict))

    def test_direct_string_hashtab_entry(self):
        b_dict = {"palette": "hashtab_entry"}
        self.assertTrue(should_pulse_red(b_dict))

    def test_direct_string_valid_palette(self):
        b_dict = {"palette": "2025-11-27-19-43-39.wav"}
        self.assertFalse(should_pulse_red(b_dict))

    def test_array_containing_dictionary_entry(self):
        b_dict = {"palette": ["2025-11-27-19-43-39.wav", "dictionary_entry"]}
        self.assertTrue(should_pulse_red(b_dict))

    def test_array_containing_hashtab_entry(self):
        b_dict = {"palette": ["hashtab_entry", "2025-11-27-19-43-39.wav"]}
        self.assertTrue(should_pulse_red(b_dict))

    def test_array_containing_empty_array(self):
        b_dict = {"palette": ["2025-11-27-19-43-39.wav", []]}
        self.assertTrue(should_pulse_red(b_dict))

    def test_array_containing_nonempty_array(self):
        b_dict = {"palette": ["2025-11-27-19-43-39.wav", ["other"]]}
        self.assertFalse(should_pulse_red(b_dict))

    def test_array_standard_valid(self):
        b_dict = {"palette": ["2025-11-27-19-43-39.wav", "2025-11-26-19-34-12.wav"]}
        self.assertFalse(should_pulse_red(b_dict))

    def test_missing_palette_key(self):
        b_dict = {"rating": 0.9}
        self.assertFalse(should_pulse_red(b_dict))

    def test_none_palette(self):
        b_dict = {"palette": None}
        self.assertFalse(should_pulse_red(b_dict))

    def test_invalid_b_dict_type(self):
        self.assertFalse(should_pulse_red(None))
        self.assertFalse(should_pulse_red([]))

if __name__ == '__main__':
    unittest.main()
