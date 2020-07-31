//+build !android

package app

func SetClipboardString(input string) error {
	return nil
}

func GetClipboardString() (string, error) {
	return "", nil
}
