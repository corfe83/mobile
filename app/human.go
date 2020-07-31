//+build !android

package app

import "errors"

func SetClipboardString(input string) error {
	return errors.New("Unsupported platform")
}

func GetClipboardString() (string, error) {
	return "", errors.New("Unsupported platform")
}

func OpenUrl(url string) error {
	return errors.New("Unsupported platform")
}