import flask

app = flask.Flask("weather_server")

@app.route("/", methods = ["POST"])
def post_recv():
	text = flask.request.get_data(as_text=True)
	print(f"Temperature: {text}°C")
	return ""

@app.route("/location", methods = ["GET"])
def location():
	return "Santa+Cruz"

if __name__ == "__main__":
	app.run(host = "0.0.0.0", port = 1234)